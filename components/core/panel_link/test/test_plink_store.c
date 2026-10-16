/*
 * test_plink_store.c - what the panel link keeps in NVS `link` (plink_store.h): the master's
 * record (its net id and its keypads) and the keypad's (its master). Round trips, damage refused,
 * a damaged keypad entry dropped while the others load, numbers outside 1..8 and duplicates
 * dropped, a newer compat refused, fields a later format appends skipped, and the frozen format-1
 * fixture (plink_store_fixture_v1.h, made independently by vectors_plink_store.py).
 * Requirements: LINK-001, LINK-017 (docs/features/11)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm/alarm_codec.h"
#include "panel_link/plink_store.h"
#include "unity.h"

#include "plink_store_fixture_v1.h"

#define HDR 11u

static uint8_t B[PLINK_STORE_MAX + 64];

void setUp(void) {}

void tearDown(void) {}

/* ---- the fixture's values ------------------------------------------------------------------ */

static void fixture_master(plink_master_rec_t *m)
{
    memset(m, 0, sizeof(*m));
    m->net_id = 0x5A3Cu;
    m->count = 2u;
    plink_peer_rec_t *p = &m->peer[0];
    p->device = 1u;
    p->home = 1u;
    p->kind = PLINK_KIND_PANEL;
    p->supervision_s = PLINK_SUPERVISION_DEFAULT_S;
    for (unsigned i = 0; i < PLINK_KEY_LEN; i++) {
        p->k_pair[i] = (uint8_t)i;
    }
    p->paired_epoch_ms = 1790000000000LL;
    strcpy(p->name, "Hall keypad");
    p = &m->peer[1];
    p->device = 3u;
    p->home = 2u;
    p->kind = PLINK_KIND_PC;
    p->flags = PLINK_PEER_PROVISIONAL;
    p->supervision_s = 300u;
    for (unsigned i = 0; i < PLINK_KEY_LEN; i++) {
        p->k_pair[i] = (uint8_t)(0xA0u + i);
    }
    strcpy(p->name, "Bureau \xC3\xA9tage");
}

static void fixture_keypad(plink_keypad_rec_t *k)
{
    memset(k, 0, sizeof(*k));
    k->net_id = 0x5A3Cu;
    k->device = 3u;
    k->home = 2u;
    for (unsigned i = 0; i < PLINK_KEY_LEN; i++) {
        k->k_pair[i] = (uint8_t)(0xA0u + i);
    }
    strcpy(k->master_name, "Harbor House");
    strcpy(k->panel_id, "3F9A1C");
}

/* ---- comparing ------------------------------------------------------------------------------ */

static void expect_peer(const plink_peer_rec_t *want, const plink_peer_rec_t *got)
{
    TEST_ASSERT_EQUAL_UINT8(want->device, got->device);
    TEST_ASSERT_EQUAL_UINT8(want->home, got->home);
    TEST_ASSERT_EQUAL_UINT8(want->kind, got->kind);
    TEST_ASSERT_EQUAL_UINT8(want->flags, got->flags);
    TEST_ASSERT_EQUAL_UINT16(want->supervision_s, got->supervision_s);
    TEST_ASSERT_EQUAL_MEMORY(want->k_pair, got->k_pair, PLINK_KEY_LEN);
    TEST_ASSERT_TRUE(want->paired_epoch_ms == got->paired_epoch_ms);
    TEST_ASSERT_EQUAL_STRING(want->name, got->name);
}

static void expect_master(const plink_master_rec_t *want, const plink_master_rec_t *got)
{
    TEST_ASSERT_EQUAL_UINT16(want->net_id, got->net_id);
    TEST_ASSERT_EQUAL_UINT8(want->count, got->count);
    for (unsigned i = 0; i < want->count; i++) {
        expect_peer(&want->peer[i], &got->peer[i]);
    }
}

static void expect_keypad(const plink_keypad_rec_t *want, const plink_keypad_rec_t *got)
{
    TEST_ASSERT_EQUAL_UINT16(want->net_id, got->net_id);
    TEST_ASSERT_EQUAL_UINT8(want->device, got->device);
    TEST_ASSERT_EQUAL_UINT8(want->home, got->home);
    TEST_ASSERT_EQUAL_MEMORY(want->k_pair, got->k_pair, PLINK_KEY_LEN);
    TEST_ASSERT_EQUAL_STRING(want->master_name, got->master_name);
    TEST_ASSERT_EQUAL_STRING(want->panel_id, got->panel_id);
}

static bool all_zero(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        if (b[i] != 0u) {
            return false;
        }
    }
    return true;
}

/* ---- building records by hand (the layout of plink_store.h, not the encoder) ---------------- */

/* A record of kind around payload[0..n) into B, header and CRC included; returns its length. */
static size_t seal(uint8_t kind, uint8_t compat, const uint8_t *payload, size_t n)
{
    const uint32_t crc = alarm_crc32(0u, payload, n);
    B[0] = 'P';
    B[1] = 'L';
    B[2] = kind;
    B[3] = 1u;
    B[4] = compat;
    B[5] = (uint8_t)n;
    B[6] = (uint8_t)(n >> 8);
    for (unsigned i = 0; i < 4u; i++) {
        B[7 + i] = (uint8_t)(crc >> (8u * i));
    }
    memmove(B + HDR, payload, n);
    return HDR + n;
}

/* Fixes the header's length and CRC after the payload in B was edited. */
static void reseal(size_t len)
{
    const size_t n = len - HDR;
    const uint32_t crc = alarm_crc32(0u, B + HDR, n);
    B[5] = (uint8_t)n;
    B[6] = (uint8_t)(n >> 8);
    for (unsigned i = 0; i < 4u; i++) {
        B[7 + i] = (uint8_t)(crc >> (8u * i));
    }
}

typedef struct {
    uint8_t b[PLINK_STORE_MAX];
    size_t n;
} raw_t;

static void raw_u8(raw_t *r, uint8_t v)
{
    r->b[r->n++] = v;
}

static void raw_bytes(raw_t *r, const void *p, size_t n)
{
    memcpy(r->b + r->n, p, n);
    r->n += n;
}

static void raw_str(raw_t *r, const char *s)
{
    const size_t n = strlen(s);
    raw_u8(r, (uint8_t)n);
    raw_bytes(r, s, n);
}

/* One master entry: its length byte, format 1's fields, then `extra` bytes a later format
 * appends (0xEE). name_len_fix != 0 replaces the name's length byte (to damage it). */
static void raw_peer(raw_t *r, uint8_t device, const char *name, size_t extra, uint8_t name_len_fix)
{
    const size_t at = r->n;
    raw_u8(r, 0u);
    raw_u8(r, device);
    raw_u8(r, 1u); /* home */
    raw_u8(r, PLINK_KIND_PANEL);
    raw_u8(r, 0u);  /* flags */
    raw_u8(r, 60u); /* supervision_s */
    raw_u8(r, 0u);
    for (unsigned i = 0; i < PLINK_KEY_LEN; i++) {
        raw_u8(r, (uint8_t)(device * 16u + i));
    }
    for (unsigned i = 0; i < 8u; i++) {
        raw_u8(r, 0u); /* paired_epoch_ms */
    }
    const size_t name_at = r->n;
    raw_str(r, name);
    if (name_len_fix != 0u) {
        r->b[name_at] = name_len_fix;
    }
    for (size_t i = 0; i < extra; i++) {
        raw_u8(r, 0xEEu);
    }
    r->b[at] = (uint8_t)(r->n - at - 1u);
}

static void raw_master_head(raw_t *r, uint16_t net_id, uint8_t count)
{
    r->n = 0u;
    raw_u8(r, (uint8_t)net_id);
    raw_u8(r, (uint8_t)(net_id >> 8));
    raw_u8(r, count);
}

/* ---- the tests ------------------------------------------------------------------------------ */

/* LINK-017: every field of both records survives a round trip, a full master included (8
 * keypads with names as long as the wire allows, multi-byte characters); it fits PLINK_STORE_MAX
 * and a buffer one byte short is refused without writing past it. */
static void test_round_trips(void)
{
    plink_master_rec_t m;
    memset(&m, 0, sizeof(m));
    m.net_id = 0xFFFEu;
    m.count = PLINK_KEYPADS_MAX;
    for (unsigned i = 0; i < PLINK_KEYPADS_MAX; i++) {
        plink_peer_rec_t *p = &m.peer[i];
        p->device = (uint8_t)(PLINK_KEYPADS_MAX - i); /* any order */
        p->home = (uint8_t)(1u + i % 4u);
        p->kind = (uint8_t)(i % 4u);
        p->flags = (uint8_t)(i & 1u);
        p->supervision_s = (uint16_t)(i == 0 ? 0u : 0xFFFFu - i);
        for (unsigned k = 0; k < PLINK_KEY_LEN; k++) {
            p->k_pair[k] = (uint8_t)(i * 37u + k * 11u + 1u);
        }
        p->paired_epoch_ms = i == 1 ? -1 : (int64_t)(1790000000000LL + i);
        memset(p->name, 'a' + (int)i, PLINK_NAME_MAX);
        memcpy(p->name, "\xE7\x8E\x8B", 3); /* a 3-byte character first */
    }
    size_t len = 0u;
    TEST_ASSERT_TRUE(plink_store_master_encode(&m, B, sizeof(B), &len));
    TEST_ASSERT_TRUE(len <= PLINK_STORE_MAX);
    plink_master_rec_t got;
    memset(&got, 0xEE, sizeof(got));
    TEST_ASSERT_TRUE(plink_store_master_decode(B, len, &got));
    expect_master(&m, &got);

    uint8_t small[PLINK_STORE_MAX + 1];
    memset(small, 0x5A, sizeof(small));
    size_t n = 0u;
    TEST_ASSERT_FALSE(plink_store_master_encode(&m, small, len - 1u, &n));
    TEST_ASSERT_EQUAL_UINT8(0x5A, small[len - 1u]); /* nothing past cap */
    TEST_ASSERT_TRUE(all_zero(small, len - 1u));    /* and what fitted, keys too, wiped */

    /* An empty master (no net id chosen yet) */
    memset(&m, 0, sizeof(m));
    TEST_ASSERT_TRUE(plink_store_master_encode(&m, B, sizeof(B), &len));
    TEST_ASSERT_EQUAL_UINT(HDR + 3u, len);
    TEST_ASSERT_TRUE(plink_store_master_decode(B, len, &got));
    expect_master(&m, &got);
    m.count = PLINK_KEYPADS_MAX + 1u;
    TEST_ASSERT_FALSE(plink_store_master_encode(&m, B, sizeof(B), &len));

    plink_keypad_rec_t k;
    fixture_keypad(&k);
    k.device = PLINK_DEVICE_LAST;
    k.home = 8u;
    memset(k.master_name, 'M', PLINK_NAME_MAX);
    TEST_ASSERT_TRUE(plink_store_keypad_encode(&k, B, sizeof(B), &len));
    TEST_ASSERT_TRUE(len <= PLINK_STORE_MAX);
    plink_keypad_rec_t kg;
    memset(&kg, 0xEE, sizeof(kg));
    TEST_ASSERT_TRUE(plink_store_keypad_decode(B, len, &kg));
    expect_keypad(&k, &kg);
    TEST_ASSERT_FALSE(plink_store_keypad_encode(&k, B, len - 1u, &n));
    TEST_ASSERT_FALSE(plink_store_keypad_encode(&k, B, HDR - 1u, &n));
    TEST_ASSERT_FALSE(plink_store_keypad_encode(NULL, B, sizeof(B), &n));
}

/* LINK-017: a record that isn't one is refused whole and the output zeroed: damage under the CRC,
 * a damaged CRC, the wrong magic or kind, a cut record, a compat newer than this firmware reads.
 * A newer format that keeps compat 1 still reads. */
static void test_damage_refused(void)
{
    plink_master_rec_t m, got;
    fixture_master(&m);
    size_t len = 0u;
    TEST_ASSERT_TRUE(plink_store_master_encode(&m, B, sizeof(B), &len));

    B[HDR + 20] ^= 0x01u; /* a key byte */
    memset(&got, 0xEE, sizeof(got));
    TEST_ASSERT_FALSE(plink_store_master_decode(B, len, &got));
    TEST_ASSERT_TRUE(all_zero(&got, sizeof(got)));
    B[HDR + 20] ^= 0x01u;
    TEST_ASSERT_TRUE(plink_store_master_decode(B, len, &got));

    B[8] ^= 0x80u; /* the CRC itself */
    TEST_ASSERT_FALSE(plink_store_master_decode(B, len, &got));
    B[8] ^= 0x80u;
    B[1] = 'X';
    TEST_ASSERT_FALSE(plink_store_master_decode(B, len, &got));
    B[1] = 'L';
    TEST_ASSERT_FALSE(plink_store_master_decode(B, len - 1u, &got)); /* cut */
    TEST_ASSERT_FALSE(plink_store_master_decode(B, HDR - 1u, &got));
    TEST_ASSERT_FALSE(plink_store_master_decode(NULL, len, &got));
    TEST_ASSERT_FALSE(plink_store_master_decode(B, len, NULL));

    plink_keypad_rec_t k;
    memset(&k, 0xEE, sizeof(k));
    TEST_ASSERT_FALSE(plink_store_keypad_decode(B, len, &k)); /* a master's record */
    TEST_ASSERT_TRUE(all_zero(&k, sizeof(k)));

    B[4] = 2u; /* needs a newer reader */
    TEST_ASSERT_FALSE(plink_store_master_decode(B, len, &got));
    TEST_ASSERT_TRUE(all_zero(&got, sizeof(got)));
    B[4] = 1u;
    B[3] = 7u; /* a newer format older readers can still read */
    TEST_ASSERT_TRUE(plink_store_master_decode(B, len, &got));
    expect_master(&m, &got);

    /* A payload too short for the master's own fields */
    const uint8_t two[2] = {0x3C, 0x5A};
    len = seal('M', 1u, two, sizeof(two));
    TEST_ASSERT_FALSE(plink_store_master_decode(B, len, &got));

    /* The keypad's: no network, a number outside 1..8, fields missing */
    plink_keypad_rec_t want;
    fixture_keypad(&want);
    TEST_ASSERT_TRUE(plink_store_keypad_encode(&want, B, sizeof(B), &len));
    TEST_ASSERT_TRUE(plink_store_keypad_decode(B, len, &k));
    TEST_ASSERT_FALSE(plink_store_master_decode(B, len, &got)); /* a keypad's record */
    static const uint8_t bad_device[] = {0, PLINK_DEVICE_LAST + 1u};
    for (unsigned i = 0; i < sizeof(bad_device); i++) {
        TEST_ASSERT_TRUE(plink_store_keypad_encode(&want, B, sizeof(B), &len));
        B[HDR + 2] = bad_device[i];
        reseal(len);
        memset(&k, 0xEE, sizeof(k));
        TEST_ASSERT_FALSE(plink_store_keypad_decode(B, len, &k));
        TEST_ASSERT_TRUE(all_zero(&k, sizeof(k)));
    }
    TEST_ASSERT_TRUE(plink_store_keypad_encode(&want, B, sizeof(B), &len));
    B[HDR] = 0u;
    B[HDR + 1] = 0u;
    reseal(len);
    TEST_ASSERT_FALSE(plink_store_keypad_decode(B, len, &k));
    TEST_ASSERT_TRUE(plink_store_keypad_encode(&want, B, sizeof(B), &len));
    len -= 7u; /* without the panel id */
    reseal(len);
    TEST_ASSERT_FALSE(plink_store_keypad_decode(B, len, &k));
    TEST_ASSERT_TRUE(all_zero(&k, sizeof(k)));
}

/* LINK-001, LINK-017: one damaged keypad entry is dropped and the others still load - a name
 * running past its entry, an entry shorter than format 1's fields, a number outside 1..8, a
 * number already taken (the first one kept); entries that end before the count say keep what
 * came before. */
static void test_damaged_entry_dropped(void)
{
    static raw_t r;
    raw_master_head(&r, 0x1234u, 7u);
    raw_peer(&r, 1u, "Kitchen", 0u, 0u);
    raw_peer(&r, 2u, "Stairs", 0u, 40u); /* its name says 40 bytes: past the entry */
    const size_t short_at = r.n;
    raw_u8(&r, 10u); /* an entry of 10 bytes: too short */
    for (unsigned i = 0; i < 10u; i++) {
        raw_u8(&r, 4u);
    }
    raw_peer(&r, 0u, "Nobody", 0u, 0u);   /* number 0 */
    raw_peer(&r, 9u, "Ninth", 0u, 0u);    /* number 9 */
    raw_peer(&r, 1u, "Impostor", 0u, 0u); /* 1 again */
    raw_peer(&r, 5u, "Garage", 0u, 0u);
    TEST_ASSERT_EQUAL_UINT8(10u, r.b[short_at]);
    size_t len = seal('M', 1u, r.b, r.n);

    plink_master_rec_t got;
    TEST_ASSERT_TRUE(plink_store_master_decode(B, len, &got));
    TEST_ASSERT_EQUAL_UINT16(0x1234u, got.net_id);
    TEST_ASSERT_EQUAL_UINT8(2u, got.count);
    TEST_ASSERT_EQUAL_UINT8(1u, got.peer[0].device);
    TEST_ASSERT_EQUAL_STRING("Kitchen", got.peer[0].name);
    TEST_ASSERT_EQUAL_UINT8(16u, got.peer[0].k_pair[0]);
    TEST_ASSERT_EQUAL_UINT8(5u, got.peer[1].device);
    TEST_ASSERT_EQUAL_STRING("Garage", got.peer[1].name);
    TEST_ASSERT_EQUAL_UINT8(5u * 16u + 31u, got.peer[1].k_pair[31]);

    /* An entry whose length runs past the payload: what came before it loads. */
    raw_master_head(&r, 0x1234u, 3u);
    raw_peer(&r, 4u, "Porch", 0u, 0u);
    const size_t office_at = r.n;
    raw_peer(&r, 6u, "Office", 0u, 0u);
    r.b[office_at] = 200u;
    len = seal('M', 1u, r.b, r.n);
    TEST_ASSERT_TRUE(plink_store_master_decode(B, len, &got));
    TEST_ASSERT_EQUAL_UINT8(1u, got.count);
    TEST_ASSERT_EQUAL_UINT8(4u, got.peer[0].device);

    /* A count larger than the entries there are */
    raw_master_head(&r, 0x0101u, 9u);
    raw_peer(&r, 8u, "Loft", 0u, 0u);
    len = seal('M', 1u, r.b, r.n);
    TEST_ASSERT_TRUE(plink_store_master_decode(B, len, &got));
    TEST_ASSERT_EQUAL_UINT8(1u, got.count);
    TEST_ASSERT_EQUAL_STRING("Loft", got.peer[0].name);
}

/* LINK-017: fields a later format appends are skipped - bytes after a keypad entry's fields,
 * bytes after the master's entries, bytes after the keypad record's fields - and a name longer
 * than this firmware holds is cut where a character starts. */
static void test_appended_fields_skipped(void)
{
    static raw_t r;
    raw_master_head(&r, 0x2468u, 2u);
    raw_peer(&r, 2u, "Back door", 9u, 0u);
    raw_peer(&r, 7u, "Shed", 1u, 0u);
    for (unsigned i = 0; i < 12u; i++) {
        raw_u8(&r, 0x77u); /* the master's own appended fields */
    }
    size_t len = seal('M', 1u, r.b, r.n);
    plink_master_rec_t got;
    TEST_ASSERT_TRUE(plink_store_master_decode(B, len, &got));
    TEST_ASSERT_EQUAL_UINT8(2u, got.count);
    TEST_ASSERT_EQUAL_UINT8(2u, got.peer[0].device);
    TEST_ASSERT_EQUAL_STRING("Back door", got.peer[0].name);
    TEST_ASSERT_EQUAL_UINT16(60u, got.peer[0].supervision_s);
    TEST_ASSERT_EQUAL_UINT8(7u, got.peer[1].device);
    TEST_ASSERT_EQUAL_STRING("Shed", got.peer[1].name);

    /* The keypad's record with appended fields, and names longer than this firmware's: 50
     * bytes whose 47th and 48th are one two-byte character, so the cut keeps 46. */
    char long_name[51];
    memset(long_name, 'n', 50);
    long_name[50] = '\0';
    long_name[46] = (char)0xC3;
    long_name[47] = (char)0xA9;
    r.n = 0u;
    raw_u8(&r, 0x3Cu);
    raw_u8(&r, 0x5Au);
    raw_u8(&r, 3u);
    raw_u8(&r, 2u);
    for (unsigned i = 0; i < PLINK_KEY_LEN; i++) {
        raw_u8(&r, (uint8_t)(0xA0u + i));
    }
    raw_str(&r, long_name);
    raw_str(&r, "3F9A1C77"); /* a longer panel id: its first 6 */
    for (unsigned i = 0; i < 20u; i++) {
        raw_u8(&r, 0x55u);
    }
    len = seal('K', 1u, r.b, r.n);
    plink_keypad_rec_t k;
    TEST_ASSERT_TRUE(plink_store_keypad_decode(B, len, &k));
    TEST_ASSERT_EQUAL_UINT16(0x5A3Cu, k.net_id);
    TEST_ASSERT_EQUAL_UINT8(3u, k.device);
    TEST_ASSERT_EQUAL_UINT8(2u, k.home);
    TEST_ASSERT_EQUAL_UINT8(0xBFu, k.k_pair[31]);
    TEST_ASSERT_EQUAL_UINT(46u, strlen(k.master_name));
    TEST_ASSERT_EQUAL_STRING("3F9A1C", k.panel_id);
}

/* LINK-017: the frozen format-1 records decode forever to these values, and this firmware still
 * writes exactly their bytes. The fixture was made independently from the layout
 * (vectors_plink_store.py) and matches what this encoder wrote; regenerate a new one with
 * DUMP_FIXTURE=1 build/host/test_plink_store when the format changes - never edit this one. */
static void test_fixture_v1(void)
{
    plink_master_rec_t m, got;
    plink_keypad_rec_t k, kg;
    fixture_master(&m);
    fixture_keypad(&k);
    size_t len = 0u;
    const char *dump = getenv("DUMP_FIXTURE");
    if (dump != NULL && strcmp(dump, "1") == 0) {
        TEST_ASSERT_TRUE(plink_store_master_encode(&m, B, sizeof(B), &len));
        printf("static const uint8_t PLINK_STORE_MASTER_V1[%u] = {", (unsigned)len);
        for (size_t i = 0; i < len; i++) {
            printf("%s0x%02X,", (i % 12u) ? " " : "\n    ", B[i]);
        }
        printf("\n};\n");
        TEST_ASSERT_TRUE(plink_store_keypad_encode(&k, B, sizeof(B), &len));
        printf("static const uint8_t PLINK_STORE_KEYPAD_V1[%u] = {", (unsigned)len);
        for (size_t i = 0; i < len; i++) {
            printf("%s0x%02X,", (i % 12u) ? " " : "\n    ", B[i]);
        }
        printf("\n};\n");
    }

    TEST_ASSERT_TRUE(
        plink_store_master_decode(PLINK_STORE_MASTER_V1, sizeof(PLINK_STORE_MASTER_V1), &got));
    expect_master(&m, &got);
    TEST_ASSERT_TRUE(plink_store_master_encode(&m, B, sizeof(B), &len));
    TEST_ASSERT_EQUAL_UINT(sizeof(PLINK_STORE_MASTER_V1), len);
    TEST_ASSERT_EQUAL_MEMORY(PLINK_STORE_MASTER_V1, B, len);

    TEST_ASSERT_TRUE(
        plink_store_keypad_decode(PLINK_STORE_KEYPAD_V1, sizeof(PLINK_STORE_KEYPAD_V1), &kg));
    expect_keypad(&k, &kg);
    TEST_ASSERT_TRUE(plink_store_keypad_encode(&k, B, sizeof(B), &len));
    TEST_ASSERT_EQUAL_UINT(sizeof(PLINK_STORE_KEYPAD_V1), len);
    TEST_ASSERT_EQUAL_MEMORY(PLINK_STORE_KEYPAD_V1, B, len);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_round_trips);
    RUN_TEST(test_damage_refused);
    RUN_TEST(test_damaged_entry_dropped);
    RUN_TEST(test_appended_fields_skipped);
    RUN_TEST(test_fixture_v1);
    return UNITY_END();
}
