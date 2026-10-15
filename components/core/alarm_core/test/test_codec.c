/*
 * test_codec.c - the versioned configuration blob (storage and backups).
 * Requirements: CORE-CFG-012..017, CORE-CFG-020, CORE-CFG-021; names in any script: I18N-009, I18N-010 (docs/features/37)
 */
#include <stdio.h>
#include <stdlib.h>

#include "alarm/alarm_codec.h"
#include "sim.h"

static alarm_config_t C;
static alarm_config_t D;
static uint8_t B[ALARM_CONFIG_BLOB_MAX + 64];

void setUp(void)
{
    sim_config_basic(&C);
    memset(&D, 0xEE, sizeof(D));
}

void tearDown(void) {}

static size_t encode(void)
{
    size_t len = 0;
    EXPECT_RESULT(ALARM_OK, alarm_config_encode(&C, B, sizeof(B), &len));
    TEST_ASSERT_TRUE(len <= ALARM_CONFIG_BLOB_MAX);
    return len;
}

static void wr32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

/* Fix the header after editing the payload. */
static void reseal(size_t len)
{
    wr32(B + 8, (uint32_t)(len - 16));
    wr32(B + 12, alarm_crc32(0, B + 16, len - 16));
}

/* Names are compared byte for byte: clear what snprintf would leave behind. */
static void set_name(char *dst, const char *name)
{
    memset(dst, 0, ALARM_NAME_LEN);
    snprintf(dst, ALARM_NAME_LEN, "%s", name);
}

/* Offset of the first item with this id in B, 0 if none. */
static size_t find_item(size_t len, uint8_t group, uint8_t field)
{
    size_t off = 16;
    while (off + 3 <= len) {
        if (B[off] == field && B[off + 1] == group) {
            return off;
        }
        off += 3u + B[off + 2];
    }
    return 0;
}

/* CORE-CFG-012: everything survives an encode/decode round trip. */
static void test_round_trip(void)
{
    C.opt.abort_window_s = 15;
    C.opt.auto_stay = false;
    C.opt.trouble_beep_mask = 0x5A5;
    C.opt.panic_keys[1] = ALARM_PANIC_SILENT;
    C.opt.power_up_program = false;
    set_name(C.partitions[0].name, "Main house");
    C.partitions[1].enabled = true;
    C.partitions[1].exit_delay_s = 90;
    C.zones[Z_WINDOW - 1].supervision_s = 3600;
    C.zones[Z_HALL_PIR - 1].cross_zone = Z_FAMILY;
    C.zones[Z_FAMILY - 1].swinger_trips = 3;
    sim_set_zone(&C, ALARM_MAX_ZONES, ALARM_ZT_PERIMETER, 2);
    C.devices[0].enabled = true;
    C.devices[0].partition = 1;
    C.devices[0].supervision_s = 600;
    set_name(C.devices[0].name, "Hall keypad");
    sim_set_user(&C, ALARM_MAX_USERS, ALARM_AUTH_GUEST, 0x03, "7777777");
    /* I18N-009: names in any script, as long as a name may be. */
    set_name(C.zones[Z_WINDOW - 1].name, "Fen\xC3\xAAtre du salon c\xC3\xB4t\xC3\xA9 jardin");
    set_name(C.users[1].name, "\xE7\x8E\x8B\xE5\xB0\x8F\xE6\x98\x8E");
    memset(C.devices[1].name, 'D', ALARM_NAME_LEN - 1);
    C.devices[1].enabled = true;

    const size_t len = encode();
    EXPECT_RESULT(ALARM_OK, alarm_config_decode(B, len, &D));
    TEST_ASSERT_EQUAL_MEMORY(&C, &D, sizeof(C));
}

/* Drops every item with this group and field from B[0..*len). */
static void drop_items(size_t *len, uint8_t group, uint8_t field)
{
    size_t off = 16;
    while (off + 3 <= *len) {
        const size_t item = 3u + B[off + 2];
        if (B[off] == field && B[off + 1] == group) {
            memmove(B + off, B + off + item, *len - off - item);
            *len -= item;
        } else {
            off += item;
        }
    }
    reseal(*len);
}

/* CORE-CFG-014, I18N-010: a name longer than firmware before 2026-09-29 could hold is written
 * twice - whole in a field that firmware skips, and cut on a whole character to its 23 bytes
 * where it looks - so a rollback keeps every name's beginning instead of refusing the blob. */
static void test_long_names_for_older_firmware(void)
{
    /* 22 letters, then a two-byte "\xC3\xBC" across the old limit: the cut goes before it. */
    set_name(C.zones[Z_WINDOW - 1].name, "Wohnzimmerfensterlinks\xC3\xBC ber der T\xC3\xBCr");
    set_name(C.partitions[0].name, "Casa de verano en la monta\xC3\xB1" "a");
    set_name(C.users[1].name, "Alex");                    /* short: one field, as before */
    size_t len = encode();
    EXPECT_RESULT(ALARM_OK, alarm_config_decode(B, len, &D));
    TEST_ASSERT_EQUAL_MEMORY(&C, &D, sizeof(C));

    /* Older firmware reads only the old fields: remove the whole names and decode. */
    drop_items(&len, 0x01, 0x06);
    drop_items(&len, 0x02, 0x0A);
    drop_items(&len, 0x03, 0x05);
    drop_items(&len, 0x04, 0x05);
    EXPECT_RESULT(ALARM_OK, alarm_config_decode(B, len, &D));
    TEST_ASSERT_EQUAL_STRING("Wohnzimmerfensterlinks", D.zones[Z_WINDOW - 1].name);
    TEST_ASSERT_EQUAL_STRING("Casa de verano en la mo", D.partitions[0].name); /* 23 bytes */
    TEST_ASSERT_EQUAL_STRING("Alex", D.users[1].name);
    for (unsigned z = 0; z < ALARM_MAX_ZONES; z++) {
        TEST_ASSERT_TRUE(strlen(D.zones[z].name) < ALARM_NAME_LEGACY_LEN);
    }

    /* The old field never carries more than such firmware takes. */
    len = encode();
    const size_t off = find_item(len, 0x01, 0x02);
    TEST_ASSERT_NOT_EQUAL(0, off);
    TEST_ASSERT_TRUE(B[off + 2] <= ALARM_NAME_LEGACY_LEN - 1u);
    /* A short name has no second field. */
    TEST_ASSERT_EQUAL(0, find_item(len, 0x03, 0x05));
}

/* CORE-CFG-013: a configuration using every slot fits ALARM_CONFIG_BLOB_MAX. */
static void test_worst_case_fits(void)
{
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        C.partitions[p].enabled = true;
        memset(C.partitions[p].name, 'P', ALARM_NAME_LEN - 1);
    }
    for (uint16_t z = 1; z <= ALARM_MAX_ZONES; z++) {
        sim_set_zone(&C, z, ALARM_ZT_PERIMETER, (uint8_t)(1 + z % ALARM_MAX_PARTITIONS));
        alarm_zone_config_t *zc = &C.zones[z - 1];
        memset(zc->name, 'Z', ALARM_NAME_LEN - 1);
        zc->flags = 0xFFFF;
        zc->user = 64;
        zc->swinger_trips = 6;
        zc->cross_zone = (uint16_t)(z % ALARM_MAX_ZONES + 1);
        zc->supervision_s = 65535;
        zc->device_class = ALARM_DEVCLASS_KEYSWITCH;
        /* CORE-CFG-020, CORE-CFG-021: a whole spoken name and all three revisions */
        for (unsigned w = 0; w < ALARM_ZONE_WORDS; w++) {
            alarm_zone_set_word(zc, w, ALARM_VOICE_TOKEN_SELECTABLE_MAX);
        }
        zc->voice_binding_revision = UINT32_MAX - 1u;
        zc->voice_approved_revision = UINT32_MAX - 1u;
        zc->voice_pack_revision = UINT32_MAX;
    }
    C.opt.voice_revision_counter = UINT32_MAX;
    for (uint8_t u = 3; u <= ALARM_MAX_USERS; u++) {
        char code[12];
        snprintf(code, sizeof(code), "%u", 5000000u + u);
        sim_set_user(&C, u, ALARM_AUTH_DURESS, 0xFF, code);
    }
    for (unsigned u = 0; u < ALARM_MAX_USERS; u++) {
        memset(C.users[u].name, 'U', ALARM_NAME_LEN - 1);
    }
    for (unsigned d = 0; d < ALARM_MAX_DEVICES; d++) {
        C.devices[d].enabled = true;
        C.devices[d].partition = 1;
        C.devices[d].supervision_s = 65535;
        memset(C.devices[d].name, 'D', ALARM_NAME_LEN - 1);
    }
    char why[96];
    TEST_ASSERT_EQUAL_INT_MESSAGE(ALARM_OK, alarm_config_validate(&C, why, sizeof(why)), why);

    const size_t len = encode();
    EXPECT_RESULT(ALARM_OK, alarm_config_decode(B, len, &D));
    TEST_ASSERT_EQUAL_MEMORY(&C, &D, sizeof(C));
    size_t n = 0;
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_encode(&C, B, len - 1, &n)); /* too small */
}

/* CORE-CFG-014: unknown items are skipped (older firmware reading a newer blob) and missing
 * items keep their defaults (newer firmware reading an older blob). */
static void test_compatible_both_ways(void)
{
    C.opt.auto_stay = false;
    size_t len = encode();

    static const uint8_t extra[] = {
        0x7F, 0x00, 2, 0xAA, 0xBB, /* unknown system field */
        0x00, 0x21, 1, 0x05,       /* unknown group: an entry selector ... */
        0x03, 0x21, 1, 0x01,       /* ... and a field */
        0x7E, 0x02, 1, 0x01,       /* unknown zone field (applies to the last zone) */
    };
    memcpy(B + len, extra, sizeof(extra));
    len += sizeof(extra);
    B[4] = 9; /* written by a newer format that older readers can still read (compat 1) */
    reseal(len);
    EXPECT_RESULT(ALARM_OK, alarm_config_decode(B, len, &D));
    TEST_ASSERT_EQUAL_MEMORY(&C, &D, sizeof(C));

    /* Drop the auto_stay item: the default (true) comes back. */
    const size_t off = find_item(len, 0x00, 0x0C);
    TEST_ASSERT_NOT_EQUAL(0, off);
    const size_t item = 3u + B[off + 2];
    memmove(B + off, B + off + item, len - off - item);
    len -= item;
    reseal(len);
    EXPECT_RESULT(ALARM_OK, alarm_config_decode(B, len, &D));
    TEST_ASSERT_TRUE(D.opt.auto_stay);
    D.opt.auto_stay = false;
    TEST_ASSERT_EQUAL_MEMORY(&C, &D, sizeof(C));
}

/* CORE-CFG-015: damaged, incompatible or invalid blobs are rejected as a whole. */
static void test_rejects_bad_blobs(void)
{
    size_t len = encode();
    alarm_config_t out;

    B[40] ^= 0x01; /* payload damage: CRC */
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_decode(B, len, &out));
    B[40] ^= 0x01;
    EXPECT_RESULT(ALARM_OK, alarm_config_decode(B, len, &out));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_decode(B, len - 1, &out)); /* truncated */
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_decode(B, 10, &out));
    B[0] = 'X';
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_decode(B, len, &out));
    B[0] = 'A';
    B[5] = ALARM_CONFIG_BLOB_COMPAT + 1; /* needs a newer reader */
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_decode(B, len, &out));
    B[5] = 1;

    /* A zone number this build can't hold. */
    size_t off = find_item(len, 0x02, 0x00);
    TEST_ASSERT_NOT_EQUAL(0, off);
    B[off + 3] = (uint8_t)((ALARM_MAX_ZONES + 1) & 0xFF);
    B[off + 4] = (uint8_t)((ALARM_MAX_ZONES + 1) >> 8);
    reseal(len);
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_decode(B, len, &out));

    /* A value too wide for its field (zone partition is one byte). */
    len = encode();
    off = find_item(len, 0x02, 0x02);
    TEST_ASSERT_NOT_EQUAL(0, off);
    memmove(B + off + 5, B + off + 4, len - off - 4);
    B[off + 2] = 2;
    B[off + 3] = 0x01;
    B[off + 4] = 0x01;
    len += 1;
    reseal(len);
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_decode(B, len, &out));

    /* Decodes fine but isn't a valid configuration: zone 1 in a disabled partition. */
    C.zones[0].partition = 2;
    C.partitions[1].enabled = true;
    len = encode();
    off = find_item(len, 0x01, 0x01); /* partition 1 "enabled" is the first; find 2's */
    off = off + 3u + B[off + 2];
    while (!(B[off] == 0x01 && B[off + 1] == 0x01)) {
        off += 3u + B[off + 2];
    }
    B[off + 3] = 0; /* partition 2 disabled */
    reseal(len);
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_decode(B, len, &out));
}

/* CORE-CFG-016: a blob written by format 1 firmware decodes forever (frozen fixtures), and this
 * firmware still writes exactly the bytes of the newest one for that configuration. When the
 * format changes, add a new fixture; never edit one. */
static void fixture_config(alarm_config_t *c)
{
    alarm_config_defaults(c);
    for (unsigned i = 0; i < ALARM_CODE_SALT_LEN; i++) {
        c->code_salt[i] = (uint8_t)(0x10u + i);
    }
    alarm_config_set_default_users(c, sim_hash, NULL);
    set_name(c->partitions[0].name, "Home");
    c->opt.bell_timeout_s = 600;
    alarm_zone_config_defaults(&c->zones[0], 1, ALARM_ZT_ENTRY_EXIT_1, 1);
    set_name(c->zones[0].name, "Front Door");
    alarm_zone_config_defaults(&c->zones[5], 6, ALARM_ZT_FIRE, 1);
    set_name(c->zones[5].name, "Kitchen Smoke");
}

#include "codec_fixture_v1.h"
#include "codec_fixture_v2.h"
#include "codec_fixture_v3.h"
#include "codec_fixture_v4.h"
#include "codec_fixture_v5.h"
#include "codec_fixture_v6.h"

/* The same configuration with names past the old 23 bytes, in several scripts (fixture v3). */
static void fixture_config_long(alarm_config_t *c)
{
    fixture_config(c);
    set_name(c->partitions[0].name, "Casa de verano en la monta\xC3\xB1" "a");
    set_name(c->zones[0].name, "Porte d\xE2\x80\x99" "entr\xC3\xA9" "e c\xC3\xB4t\xC3\xA9 rue");
    set_name(c->zones[5].name, "K\xC3\xBC" "chenrauchmelder im Erdgeschoss");
    set_name(c->users[1].name, "\xE7\x8E\x8B\xE5\xB0\x8F\xE6\x98\x8E \xE5\xAE\xB6\xE9\x95\xBF");
}

/* The oldest blob: written before the chime option existed. Decoding it must still give this
 * configuration, the missing field keeping its default (chime asks for a code, CORE-CHIME-005). */
static void test_fixture_v1_still_decodes(void)
{
    fixture_config(&C);
    EXPECT_RESULT(ALARM_OK, alarm_config_decode(FIXTURE_V1, sizeof(FIXTURE_V1), &D));
    TEST_ASSERT_EQUAL_MEMORY(&C, &D, sizeof(C));
    TEST_ASSERT_TRUE(D.opt.chime_needs_code);
}

/* Fixture v2 (2026-09, before "Allow less secure automations", 0x1D): it still decodes to the same
 * configuration, the missing option keeping its default - on (docs/features/41 AUT-015). */
static void test_format_1_fixture(void)
{
    fixture_config(&C);
    EXPECT_RESULT(ALARM_OK, alarm_config_decode(FIXTURE_V2, sizeof(FIXTURE_V2), &D));
    TEST_ASSERT_EQUAL_MEMORY(&C, &D, sizeof(C));
    TEST_ASSERT_TRUE(D.opt.auto_less_secure);
}

/* CORE-CFG-016, I18N-009: fixture v3, whole names (2026-09-29), written before 0x1D: it still
 * decodes, and the option it lacks reads as on (AUT-015). */
static void test_long_names_fixture(void)
{
    fixture_config_long(&C);
    EXPECT_RESULT(ALARM_OK, alarm_config_decode(FIXTURE_V3, sizeof(FIXTURE_V3), &D));
    TEST_ASSERT_EQUAL_MEMORY(&C, &D, sizeof(C));
    TEST_ASSERT_TRUE(D.opt.auto_less_secure);
}

/* CORE-CFG-016, AUT-015: format 1 with "Allow less secure automations" (0x1D, 2026-10-05),
 * frozen like the others; this firmware writes exactly its bytes. Regenerate a new one with
 * DUMP_FIXTURE=4. */
static void test_less_secure_fixture(void)
{
    fixture_config_long(&C);
    const char *dump = getenv("DUMP_FIXTURE");
    if (dump != NULL && strcmp(dump, "4") == 0) {
        const size_t n = encode();
        printf("static const uint8_t FIXTURE_V4[%u] = {", (unsigned)n);
        for (size_t i = 0; i < n; i++) {
            printf("%s0x%02X,", (i % 12) ? " " : "\n    ", B[i]);
        }
        printf("\n};\n");
    }
    EXPECT_RESULT(ALARM_OK, alarm_config_decode(FIXTURE_V4, sizeof(FIXTURE_V4), &D));
    TEST_ASSERT_EQUAL_MEMORY(&C, &D, sizeof(C));
    const size_t len = encode();
    TEST_ASSERT_EQUAL_UINT(sizeof(FIXTURE_V4), len);
    TEST_ASSERT_EQUAL_MEMORY(FIXTURE_V4, B, len);
}

/* CORE-CFG-016, AUT-015: the short configuration (fixture_config, fixture v2's) with 0x1D, frozen
 * as v5 (2026-10-05): this firmware writes exactly its bytes, so the encoder's output stays
 * pinned for a short configuration as v4 pins it for a long one. Regenerate a new one with
 * DUMP_FIXTURE=5. */
static void test_short_less_secure_fixture(void)
{
    fixture_config(&C);
    const char *dump = getenv("DUMP_FIXTURE");
    if (dump != NULL && strcmp(dump, "5") == 0) {
        const size_t n = encode();
        printf("static const uint8_t FIXTURE_V5[%u] = {", (unsigned)n);
        for (size_t i = 0; i < n; i++) {
            printf("%s0x%02X,", (i % 12) ? " " : "\n    ", B[i]);
        }
        printf("\n};\n");
    }
    EXPECT_RESULT(ALARM_OK, alarm_config_decode(FIXTURE_V5, sizeof(FIXTURE_V5), &D));
    TEST_ASSERT_EQUAL_MEMORY(&C, &D, sizeof(C));
    TEST_ASSERT_TRUE(D.opt.auto_less_secure);
    const size_t len = encode();
    TEST_ASSERT_EQUAL_UINT(sizeof(FIXTURE_V5), len);
    TEST_ASSERT_EQUAL_MEMORY(FIXTURE_V5, B, len);
    /* v5 is v2 and the option's 4 bytes (id 0x1D 0x00, length 1, on) right after the chime's
     * (0x1C), with the header's length and CRC to match */
    TEST_ASSERT_EQUAL_UINT(sizeof(FIXTURE_V2) + 4u, sizeof(FIXTURE_V5));
    static const uint8_t opts[] = {0x1C, 0x00, 0x01, 0x01, 0x1D, 0x00, 0x01, 0x01};
    size_t at = 16u;
    while (at + sizeof opts <= sizeof(FIXTURE_V5) && memcmp(FIXTURE_V5 + at, opts, sizeof opts)) {
        at++;
    }
    TEST_ASSERT_LESS_THAN_UINT(sizeof(FIXTURE_V5), at + sizeof opts);
    TEST_ASSERT_EQUAL_MEMORY(FIXTURE_V2 + 16, FIXTURE_V5 + 16, at + 4u - 16u);
    TEST_ASSERT_EQUAL_MEMORY(FIXTURE_V2 + at + 4u, FIXTURE_V5 + at + 8u,
                             sizeof(FIXTURE_V2) - at - 4u);
}

/* Fixture v5's configuration with spoken names (fixture v6): zone 1 approved, words 0x0102, 3, 3
 * (a word past one byte, a repeat), zone 6 a draft, never approved. */
static void fixture_config_voice(alarm_config_t *c)
{
    fixture_config(c);
    alarm_zone_config_t *z = &c->zones[0];
    alarm_zone_set_word(z, 0, 0x0102u);
    alarm_zone_set_word(z, 1, 3u);
    alarm_zone_set_word(z, 2, 3u);
    z->voice_binding_revision = 2u;
    z->voice_approved_revision = 2u;
    z->voice_pack_revision = 5u;
    z = &c->zones[5];
    alarm_zone_set_word(z, 0, 200u);
    z->voice_binding_revision = 1u;
    c->opt.voice_revision_counter = 2u;
}

/* CORE-CFG-016, CORE-CFG-020, CORE-CFG-021: format 1 with the zones' spoken names (0x0B) and the
 * voice revisions (system 0x1E, zone 0x0C..0x0E), frozen as v6 (2026-10-08). This firmware
 * writes exactly its bytes; the token IDs are little-endian whatever the host; and an older
 * reader, which skips the new ids, sees exactly fixture v5. Regenerate a new one with
 * DUMP_FIXTURE=6. */
static void test_voice_fixture(void)
{
    fixture_config_voice(&C);
    const char *dump = getenv("DUMP_FIXTURE");
    if (dump != NULL && strcmp(dump, "6") == 0) {
        const size_t n = encode();
        printf("static const uint8_t FIXTURE_V6[%u] = {", (unsigned)n);
        for (size_t i = 0; i < n; i++) {
            printf("%s0x%02X,", (i % 12) ? " " : "\n    ", B[i]);
        }
        printf("\n};\n");
    }
    EXPECT_RESULT(ALARM_OK, alarm_config_decode(FIXTURE_V6, sizeof(FIXTURE_V6), &D));
    TEST_ASSERT_EQUAL_MEMORY(&C, &D, sizeof(C));
    TEST_ASSERT_EQUAL_UINT16(0x0102u, alarm_zone_word(&D.zones[0], 0));
    TEST_ASSERT_EQUAL_UINT(3u, alarm_zone_word_count(&D.zones[0]));
    TEST_ASSERT_TRUE(alarm_zone_voice_approved(&D.zones[0]));
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(&D.zones[5]));
    size_t len = encode();
    TEST_ASSERT_EQUAL_UINT(sizeof(FIXTURE_V6), len);
    TEST_ASSERT_EQUAL_MEMORY(FIXTURE_V6, B, len);

    /* zone 1's words: exactly 16 bytes, 0x0102 as 02 01, then 03 00 03 00 and zeros */
    const size_t off = find_item(len, 0x02, 0x0B);
    TEST_ASSERT_NOT_EQUAL(0, off);
    TEST_ASSERT_EQUAL_UINT8(16, B[off + 2]);
    static const uint8_t words[16] = {0x02, 0x01, 0x03, 0x00, 0x03, 0x00};
    TEST_ASSERT_EQUAL_MEMORY(words, B + off + 3, sizeof(words));

    /* What an older reader sees once it has skipped the ids it doesn't know: fixture v5. */
    drop_items(&len, 0x00, 0x1E);
    for (uint8_t f = 0x0B; f <= 0x0E; f++) {
        drop_items(&len, 0x02, f);
    }
    TEST_ASSERT_EQUAL_UINT(sizeof(FIXTURE_V5), len);
    TEST_ASSERT_EQUAL_MEMORY(FIXTURE_V5, B, len);
}

/* CORE-CFG-020, CORE-CFG-015: a malformed spoken name refuses the blob - a word list of the wrong
 * length, a hole, a word that can't be chosen; a used zone without the fields reads zeros. */
static void test_voice_fields_rejected(void)
{
    alarm_config_t out;
    memcpy(B, FIXTURE_V6, sizeof(FIXTURE_V6));
    size_t len = sizeof(FIXTURE_V6);
    size_t off = find_item(len, 0x02, 0x0B);
    TEST_ASSERT_NOT_EQUAL(0, off);
    /* 15 bytes instead of 16 */
    B[off + 2] = 15;
    memmove(B + off + 18, B + off + 19, len - off - 19);
    len -= 1;
    reseal(len);
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_decode(B, len, &out));

    /* a hole: word 2 is 0, word 3 isn't */
    memcpy(B, FIXTURE_V6, sizeof(FIXTURE_V6));
    len = sizeof(FIXTURE_V6);
    B[off + 3 + 2] = 0;
    B[off + 3 + 3] = 0;
    reseal(len);
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_decode(B, len, &out));

    /* a status word (0x8001), which a name can't choose */
    memcpy(B, FIXTURE_V6, sizeof(FIXTURE_V6));
    B[off + 3] = 0x01;
    B[off + 4] = 0x80;
    reseal(len);
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_decode(B, len, &out));

    /* a revision wider than 4 bytes */
    memcpy(B, FIXTURE_V6, sizeof(FIXTURE_V6));
    len = sizeof(FIXTURE_V6);
    off = find_item(len, 0x02, 0x0C);
    TEST_ASSERT_NOT_EQUAL(0, off);
    memmove(B + off + 8, B + off + 7, len - off - 7);
    B[off + 2] = 5;
    B[off + 7] = 1;
    len += 1;
    reseal(len);
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_decode(B, len, &out));

    /* A configuration without spoken names writes none of the new ids. */
    fixture_config(&C);
    len = encode();
    TEST_ASSERT_EQUAL(0, find_item(len, 0x00, 0x1E));
    for (uint8_t f = 0x0B; f <= 0x0E; f++) {
        TEST_ASSERT_EQUAL(0, find_item(len, 0x02, f));
    }
}

/* AUT-015: "Allow less secure automations" is on by default and survives a round trip either
 * way. */
static void test_auto_less_secure_option(void)
{
    alarm_config_defaults(&C);
    TEST_ASSERT_TRUE(C.opt.auto_less_secure);
    fixture_config(&C);
    C.opt.auto_less_secure = false;
    EXPECT_RESULT(ALARM_OK, alarm_config_decode(B, encode(), &D));
    TEST_ASSERT_FALSE(D.opt.auto_less_secure);
    TEST_ASSERT_EQUAL_MEMORY(&C, &D, sizeof(C));
    C.opt.auto_less_secure = true;
    EXPECT_RESULT(ALARM_OK, alarm_config_decode(B, encode(), &D));
    TEST_ASSERT_TRUE(D.opt.auto_less_secure);
}

/* CORE-CFG-017: CRC-32 is the standard one (zlib), and chains. */
static void test_crc32(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, alarm_crc32(0, "123456789", 9));
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, alarm_crc32(alarm_crc32(0, "1234", 4), "56789", 5));
    TEST_ASSERT_EQUAL_HEX32(0u, alarm_crc32(0, "", 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_round_trip);
    RUN_TEST(test_long_names_for_older_firmware);
    RUN_TEST(test_worst_case_fits);
    RUN_TEST(test_compatible_both_ways);
    RUN_TEST(test_rejects_bad_blobs);
    RUN_TEST(test_fixture_v1_still_decodes);
    RUN_TEST(test_format_1_fixture);
    RUN_TEST(test_long_names_fixture);
    RUN_TEST(test_less_secure_fixture);
    RUN_TEST(test_short_less_secure_fixture);
    RUN_TEST(test_voice_fixture);
    RUN_TEST(test_voice_fields_rejected);
    RUN_TEST(test_auto_less_secure_option);
    RUN_TEST(test_crc32);
    return UNITY_END();
}
