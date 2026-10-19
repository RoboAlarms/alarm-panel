/* LINK-050..052 (docs/features/11 "Settings fields"), HAI-017 (docs/features/23): a panel's
 * Screen and sound settings as fields - the frozen table (ids, sizes, ranges, JSON keys), the
 * TLV list on the panel link and its CRC, the JSON values and the settings_set parser; every
 * field refused alone, unknown ids and keys skipped or named, and random input never read out of
 * bounds. */
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "alarm_proto/proto_prefs.h"

void setUp(void) {}
void tearDown(void) {}

static uint64_t s_seed;

static uint32_t rnd(void)
{
    s_seed = s_seed * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(s_seed >> 33);
}

/* panel_prefs_defaults() as fields. */
static proto_prefs_values_t defaults(void)
{
    proto_prefs_values_t v;
    memset(&v, 0, sizeof v);
    v.have = PROTO_PREFS_KNOWN;
    v.brightness = 80u;
    v.saver_brightness = 25u;
    v.saver_after_ms = 60000u;
    v.off_after_ms = 0u;
    v.night_on = false;
    v.night_start = 22u * 60u;
    v.night_end = 7u * 60u;
    v.night_brightness = 20u;
    v.night_off_after_ms = 60000u;
    v.clock_mode = 0u;
    v.appearance = 0u;
    v.high_contrast = false;
    v.volume = 70u;
    v.chime_volume = 70u;
    v.voice_volume = 70u;
    v.key_click = true;
    v.voice_enabled = true;
    v.voice_status = true;
    v.voice_chime = true;
    return v;
}

/* The defaults' fields section body, as the keypad encodes it (written out by hand). */
static const uint8_t GOLDEN[] = {
    19u,                               /* count */
    1u,  1u, 80u,                      /* brightness */
    2u,  1u, 25u,                      /* saver_brightness */
    3u,  4u, 0x60u, 0xEAu, 0u, 0u,     /* saver_after_ms 60000 */
    4u,  4u, 0u, 0u, 0u, 0u,           /* off_after_ms 0 (never) */
    5u,  1u, 0u,                       /* night_on */
    6u,  2u, 0x28u, 0x05u,             /* night_start 1320 (22:00) */
    7u,  2u, 0xA4u, 0x01u,             /* night_end 420 (07:00) */
    8u,  1u, 20u,                      /* night_brightness */
    9u,  4u, 0x60u, 0xEAu, 0u, 0u,     /* night_off_after_ms 60000 */
    10u, 1u, 0u,                       /* clock_mode */
    11u, 1u, 0u,                       /* appearance */
    12u, 1u, 0u,                       /* high_contrast */
    13u, 1u, 70u,                      /* volume */
    14u, 1u, 70u,                      /* chime_volume */
    15u, 1u, 70u,                      /* voice_volume */
    16u, 1u, 1u,                       /* key_click */
    17u, 1u, 1u,                       /* voice_enabled */
    18u, 1u, 1u,                       /* voice_status */
    19u, 1u, 1u,                       /* voice_chime */
};
/* zlib.crc32(GOLDEN), computed outside this code. */
#define GOLDEN_CRC 0x31DE243Eu

static const char GOLDEN_JSON[] =
    "{\"brightness\":80,\"saver_level\":25,\"saver_after\":60,\"off_after\":0,\"night\":false,"
    "\"night_start\":1320,\"night_end\":420,\"night_level\":20,\"night_off\":60,\"clock\":0,"
    "\"appearance\":0,\"contrast\":false,\"beeps\":70,\"chime_vol\":70,\"voice_vol\":70,"
    "\"key_click\":true,\"voice\":true,\"voice_stat\":true,\"voice_chime\":true}";

/* A random value in the field's range (a delay a whole number of seconds when whole_s). */
static uint32_t random_value(const proto_prefs_field_t *f, bool whole_s)
{
    if ((f->flags & PROTO_PREFS_FL_DELAY) != 0u) {
        if (rnd() % 4u == 0u) {
            return 0u;
        }
        const uint32_t s = f->min / 1000u + rnd() % (f->max / 1000u - f->min / 1000u + 1u);
        return whole_s ? s * 1000u : f->min + rnd() % (f->max - f->min + 1u);
    }
    return f->min + rnd() % (f->max - f->min + 1u);
}

static proto_prefs_values_t random_values(proto_prefs_mask_t have, bool whole_s)
{
    proto_prefs_values_t v;
    memset(&v, 0, sizeof v);
    for (unsigned id = 1u; id <= PROTO_PREFS_F_LAST; id++) {
        if ((have & PROTO_PREFS_BIT(id)) != 0u) {
            const uint32_t x = random_value(proto_prefs_field(id), whole_s);
            TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_NONE, proto_prefs_put(&v, id, x));
        }
    }
    TEST_ASSERT_EQUAL_HEX32(have, v.have);
    return v;
}

static void assert_same_values(const proto_prefs_values_t *a, const proto_prefs_values_t *b)
{
    TEST_ASSERT_EQUAL_HEX32(a->have, b->have);
    for (unsigned id = 1u; id <= PROTO_PREFS_F_LAST; id++) {
        uint32_t x = 0u, y = 0u;
        TEST_ASSERT_EQUAL(proto_prefs_get(a, id, &x), proto_prefs_get(b, id, &y));
        TEST_ASSERT_EQUAL_UINT32(x, y);
    }
}

/* Taken fields are in range, refused ones carry a why, and nothing is both. */
static void assert_sane(const proto_prefs_values_t *v, proto_prefs_mask_t refused,
                        const uint8_t why[PROTO_PREFS_F_LAST])
{
    TEST_ASSERT_EQUAL_HEX32(0u, v->have & ~PROTO_PREFS_KNOWN);
    TEST_ASSERT_EQUAL_HEX32(0u, refused & ~PROTO_PREFS_KNOWN);
    TEST_ASSERT_EQUAL_HEX32(0u, v->have & refused);
    for (unsigned id = 1u; id <= PROTO_PREFS_F_LAST; id++) {
        uint32_t x;
        if (proto_prefs_get(v, id, &x)) {
            TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_NONE, proto_prefs_check(id, x));
        }
        const bool is_refused = (refused & PROTO_PREFS_BIT(id)) != 0u;
        TEST_ASSERT_EQUAL(is_refused, why[id - 1u] != 0u);
        TEST_ASSERT_TRUE(why[id - 1u] <= PROTO_PREFS_WHY_LAST);
    }
}

/* LINK-050, LINK-052, HAI-017: the frozen table - ids, wire sizes, ranges, JSON keys and the look
 * fields never change; nothing else is a settings field. */
static void test_frozen_table(void)
{
    static const struct {
        uint8_t id, size, flags;
        const char *key;
        uint32_t min, max;
    } T[] = {
        {1u, 1u, PROTO_PREFS_FL_LOOK, "brightness", 10u, 100u},
        {2u, 1u, PROTO_PREFS_FL_LOOK, "saver_level", 0u, 50u},
        {3u, 4u, PROTO_PREFS_FL_DELAY, "saver_after", 10000u, 86400000u},
        {4u, 4u, PROTO_PREFS_FL_DELAY, "off_after", 10000u, 86400000u},
        {5u, 1u, PROTO_PREFS_FL_SWITCH | PROTO_PREFS_FL_LOOK, "night", 0u, 1u},
        {6u, 2u, PROTO_PREFS_FL_LOOK, "night_start", 0u, 1439u},
        {7u, 2u, PROTO_PREFS_FL_LOOK, "night_end", 0u, 1439u},
        {8u, 1u, PROTO_PREFS_FL_LOOK, "night_level", 10u, 100u},
        {9u, 4u, PROTO_PREFS_FL_DELAY, "night_off", 10000u, 86400000u},
        {10u, 1u, 0u, "clock", 0u, 2u},
        {11u, 1u, PROTO_PREFS_FL_LOOK, "appearance", 0u, 2u},
        {12u, 1u, PROTO_PREFS_FL_SWITCH | PROTO_PREFS_FL_LOOK, "contrast", 0u, 1u},
        {13u, 1u, 0u, "beeps", 0u, 100u},
        {14u, 1u, 0u, "chime_vol", 0u, 100u},
        {15u, 1u, 0u, "voice_vol", 0u, 100u},
        {16u, 1u, PROTO_PREFS_FL_SWITCH, "key_click", 0u, 1u},
        {17u, 1u, PROTO_PREFS_FL_SWITCH, "voice", 0u, 1u},
        {18u, 1u, PROTO_PREFS_FL_SWITCH, "voice_stat", 0u, 1u},
        {19u, 1u, PROTO_PREFS_FL_SWITCH, "voice_chime", 0u, 1u},
    };
    TEST_ASSERT_EQUAL_UINT(PROTO_PREFS_F_LAST, sizeof T / sizeof T[0]);
    proto_prefs_mask_t known = 0u, look = 0u;
    unsigned tlv = 0u;
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        const proto_prefs_field_t *f = proto_prefs_field(T[i].id);
        TEST_ASSERT_NOT_NULL(f);
        TEST_ASSERT_EQUAL_UINT8(T[i].id, f->id);
        TEST_ASSERT_EQUAL_UINT8(T[i].size, f->size);
        TEST_ASSERT_EQUAL_HEX8(T[i].flags, f->flags);
        TEST_ASSERT_EQUAL_STRING(T[i].key, f->key);
        TEST_ASSERT_EQUAL_UINT32(T[i].min, f->min);
        TEST_ASSERT_EQUAL_UINT32(T[i].max, f->max);
        TEST_ASSERT_EQUAL_UINT8(T[i].id, proto_prefs_id(T[i].key));
        /* HAI-017: every key is 11 bytes or less, and no two are alike. */
        TEST_ASSERT_TRUE(strlen(f->key) >= 1u && strlen(f->key) <= PROTO_PREFS_KEY_MAX);
        for (unsigned j = 0; j < i; j++) {
            TEST_ASSERT_TRUE(strcmp(T[j].key, f->key) != 0);
        }
        known |= PROTO_PREFS_BIT(f->id);
        look |= (f->flags & PROTO_PREFS_FL_LOOK) != 0u ? PROTO_PREFS_BIT(f->id) : 0u;
        tlv += 2u + f->size;
    }
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_KNOWN, known);
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_LOOK, look);
    TEST_ASSERT_EQUAL_HEX32(0x00000CF3u, PROTO_PREFS_LOOK); /* ids 1, 2, 5-8, 11, 12 */
    TEST_ASSERT_EQUAL_UINT(68u, tlv);
    TEST_ASSERT_EQUAL_UINT(PROTO_PREFS_TLV_MAX, tlv);
    TEST_ASSERT_EQUAL_UINT(PROTO_PREFS_TLV_MAX + 1u, PROTO_PREFS_ENC_MAX);
    TEST_ASSERT_NULL(proto_prefs_field(0u));
    TEST_ASSERT_NULL(proto_prefs_field(PROTO_PREFS_F_LAST + 1u));
    TEST_ASSERT_NULL(proto_prefs_field(255u));
    /* LINK-052: what is never a settings field has no key. */
    static const char *const NEVER[] = {"lang", "language", "region", "code", "name", "home",
                                        "partition", "role", "wifi", "ip", "time", "tz", ""};
    for (unsigned i = 0; i < sizeof NEVER / sizeof NEVER[0]; i++) {
        TEST_ASSERT_EQUAL_UINT8(0u, proto_prefs_id(NEVER[i]));
    }
    TEST_ASSERT_EQUAL_UINT8(0u, proto_prefs_id(NULL));
    /* The enum's numbers are the ids. */
    TEST_ASSERT_EQUAL_INT(1, PROTO_PREFS_F_BRIGHTNESS);
    TEST_ASSERT_EQUAL_INT(2, PROTO_PREFS_F_SAVER_BRIGHTNESS);
    TEST_ASSERT_EQUAL_INT(3, PROTO_PREFS_F_SAVER_AFTER);
    TEST_ASSERT_EQUAL_INT(4, PROTO_PREFS_F_OFF_AFTER);
    TEST_ASSERT_EQUAL_INT(5, PROTO_PREFS_F_NIGHT_ON);
    TEST_ASSERT_EQUAL_INT(6, PROTO_PREFS_F_NIGHT_START);
    TEST_ASSERT_EQUAL_INT(7, PROTO_PREFS_F_NIGHT_END);
    TEST_ASSERT_EQUAL_INT(8, PROTO_PREFS_F_NIGHT_BRIGHTNESS);
    TEST_ASSERT_EQUAL_INT(9, PROTO_PREFS_F_NIGHT_OFF_AFTER);
    TEST_ASSERT_EQUAL_INT(10, PROTO_PREFS_F_CLOCK_MODE);
    TEST_ASSERT_EQUAL_INT(11, PROTO_PREFS_F_APPEARANCE);
    TEST_ASSERT_EQUAL_INT(12, PROTO_PREFS_F_HIGH_CONTRAST);
    TEST_ASSERT_EQUAL_INT(13, PROTO_PREFS_F_VOLUME);
    TEST_ASSERT_EQUAL_INT(14, PROTO_PREFS_F_CHIME_VOLUME);
    TEST_ASSERT_EQUAL_INT(15, PROTO_PREFS_F_VOICE_VOLUME);
    TEST_ASSERT_EQUAL_INT(16, PROTO_PREFS_F_KEY_CLICK);
    TEST_ASSERT_EQUAL_INT(17, PROTO_PREFS_F_VOICE_ENABLED);
    TEST_ASSERT_EQUAL_INT(18, PROTO_PREFS_F_VOICE_STATUS);
    TEST_ASSERT_EQUAL_INT(19, PROTO_PREFS_F_VOICE_CHIME);
}

/* LINK-051, HAI-017: the why numbers and their words are frozen; an unknown number reads as
 * not allowed. Every word is one of the six settings_result's "refused" documents
 * (docs/features/23 protocol.md): BAD has none of its own and reads as not allowed. */
static void test_why(void)
{
    static const char *const WORD[] = {"", "not_allowed", "out_of_range", "in_use", "held",
                                       "voice_not_ready", "not_allowed", "busy"};
    static const char *const REFUSED[] = {"not_allowed", "out_of_range", "in_use",
                                          "held", "voice_not_ready", "busy"};
    for (unsigned w = 0u; w < 256u; w++) {
        const char *word = proto_prefs_why_word((uint8_t)w);
        bool listed = false;
        for (unsigned i = 0; i < sizeof REFUSED / sizeof REFUSED[0]; i++) {
            listed = listed || strcmp(REFUSED[i], word) == 0;
        }
        TEST_ASSERT_TRUE_MESSAGE(listed, word);
    }
    TEST_ASSERT_EQUAL_INT(1, PROTO_PREFS_WHY_NOT_ALLOWED);
    TEST_ASSERT_EQUAL_INT(2, PROTO_PREFS_WHY_RANGE);
    TEST_ASSERT_EQUAL_INT(3, PROTO_PREFS_WHY_IN_USE);
    TEST_ASSERT_EQUAL_INT(4, PROTO_PREFS_WHY_HELD);
    TEST_ASSERT_EQUAL_INT(5, PROTO_PREFS_WHY_VOICE_NOT_READY);
    TEST_ASSERT_EQUAL_INT(6, PROTO_PREFS_WHY_BAD);
    TEST_ASSERT_EQUAL_INT(7, PROTO_PREFS_WHY_BUSY);
    for (unsigned w = 1u; w <= PROTO_PREFS_WHY_LAST; w++) {
        TEST_ASSERT_EQUAL_INT((int)w, proto_prefs_why_read((uint8_t)w));
        TEST_ASSERT_EQUAL_STRING(WORD[w], proto_prefs_why_word((uint8_t)w));
    }
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_NOT_ALLOWED, proto_prefs_why_read(0u));
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_NOT_ALLOWED, proto_prefs_why_read(8u));
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_NOT_ALLOWED, proto_prefs_why_read(20u));
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_NOT_ALLOWED, proto_prefs_why_read(255u));
    TEST_ASSERT_EQUAL_STRING("not_allowed", proto_prefs_why_word(0u));
    TEST_ASSERT_EQUAL_STRING("not_allowed", proto_prefs_why_word(20u));
}

/* LINK-050: the defaults encode to the golden bytes in id order; prefs_crc is the CRC-32 of
 * those bytes (a known answer computed outside this code). */
static void test_golden_and_crc(void)
{
    const proto_prefs_values_t v = defaults();
    uint8_t buf[PROTO_PREFS_ENC_MAX];
    const size_t n = proto_prefs_encode(&v, PROTO_PREFS_KNOWN, buf, sizeof buf);
    TEST_ASSERT_EQUAL_UINT(sizeof GOLDEN, n);
    TEST_ASSERT_EQUAL_UINT(PROTO_PREFS_ENC_MAX, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(GOLDEN, buf, sizeof GOLDEN);
    TEST_ASSERT_EQUAL_HEX32(GOLDEN_CRC, proto_prefs_crc(GOLDEN, sizeof GOLDEN));
    TEST_ASSERT_EQUAL_HEX32(GOLDEN_CRC, proto_prefs_values_crc(&v));
    /* The standard check value, and an empty list. */
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, proto_prefs_crc((const uint8_t *)"123456789", 9u));
    proto_prefs_values_t none;
    memset(&none, 0, sizeof none);
    TEST_ASSERT_EQUAL_HEX32(0xD202EF8Du, proto_prefs_values_crc(&none)); /* the body {0} */
    /* One field changed: another CRC. */
    proto_prefs_values_t w = v;
    w.brightness = 81u;
    TEST_ASSERT_TRUE(proto_prefs_values_crc(&w) != GOLDEN_CRC);
    /* The master's CRC over what it received equals the keypad's own (LINK-050). */
    proto_prefs_decoded_t d;
    TEST_ASSERT_TRUE(proto_prefs_decode(GOLDEN, sizeof GOLDEN, PROTO_PREFS_KNOWN, &d));
    assert_same_values(&v, &d.values);
    TEST_ASSERT_EQUAL_HEX32(GOLDEN_CRC, proto_prefs_values_crc(&d.values));
}

/* LINK-050, LINK-051: random values and subsets go round the TLV list unchanged; a mask picks
 * the fields; a buffer too small gives 0. */
static void test_tlv_round_trip(void)
{
    s_seed = 0x5E77u;
    for (unsigned iter = 0; iter < 5000u; iter++) {
        const proto_prefs_mask_t have = rnd() & PROTO_PREFS_KNOWN;
        const proto_prefs_values_t v = random_values(have, false);
        const proto_prefs_mask_t mask = iter % 2u == 0u ? PROTO_PREFS_KNOWN : (rnd() | 0x80000000u);
        uint8_t buf[PROTO_PREFS_ENC_MAX];
        const size_t n = proto_prefs_encode(&v, mask, buf, sizeof buf);
        TEST_ASSERT_TRUE(n >= 1u && n <= PROTO_PREFS_ENC_MAX);
        proto_prefs_decoded_t d;
        TEST_ASSERT_TRUE(proto_prefs_decode(buf, n, PROTO_PREFS_KNOWN, &d));
        TEST_ASSERT_EQUAL_HEX32(have & mask & PROTO_PREFS_KNOWN, d.values.have);
        TEST_ASSERT_EQUAL_HEX32(0u, d.refused);
        TEST_ASSERT_EQUAL_UINT8(0u, d.unknown_n);
        for (unsigned id = 1u; id <= PROTO_PREFS_F_LAST; id++) {
            uint32_t x, y;
            if (proto_prefs_get(&d.values, id, &y)) {
                TEST_ASSERT_TRUE(proto_prefs_get(&v, id, &x));
                TEST_ASSERT_EQUAL_UINT32(x, y);
            }
        }
        /* Every shorter buffer is refused whole, never cut. */
        if (n > 1u) {
            TEST_ASSERT_EQUAL_UINT(0u, proto_prefs_encode(&v, mask, buf, n - 1u));
        }
    }
    const proto_prefs_values_t v = defaults();
    uint8_t one[1];
    TEST_ASSERT_EQUAL_UINT(0u, proto_prefs_encode(&v, PROTO_PREFS_KNOWN, one, 0u));
    TEST_ASSERT_EQUAL_UINT(1u, proto_prefs_encode(&v, 0u, one, sizeof one));
    TEST_ASSERT_EQUAL_HEX8(0u, one[0]);
    /* A mask bit beyond this version's ids sends nothing more. */
    uint8_t buf[PROTO_PREFS_ENC_MAX + 8u];
    TEST_ASSERT_EQUAL_UINT(PROTO_PREFS_ENC_MAX,
                           proto_prefs_encode(&v, 0xFFFFFFFFu, buf, sizeof buf));
}

/* LINK-051: a known id with the wrong length is refused BAD for that field alone; the fields
 * around it are taken. */
static void test_wrong_length(void)
{
    static const uint8_t IN[] = {
        4u,
        1u, 2u, 50u, 0u,       /* brightness as two bytes */
        13u, 1u, 30u,          /* volume */
        3u, 2u, 0x10u, 0x27u,  /* saver_after_ms as two bytes */
        6u, 0u,                /* night_start with no value */
    };
    proto_prefs_decoded_t d;
    TEST_ASSERT_TRUE(proto_prefs_decode(IN, sizeof IN, PROTO_PREFS_KNOWN, &d));
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_BIT(PROTO_PREFS_F_VOLUME), d.values.have);
    TEST_ASSERT_EQUAL_UINT8(30u, d.values.volume);
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_BIT(1u) | PROTO_PREFS_BIT(3u) | PROTO_PREFS_BIT(6u),
                            d.refused);
    TEST_ASSERT_EQUAL_UINT8(PROTO_PREFS_WHY_BAD, d.why[0]);
    TEST_ASSERT_EQUAL_UINT8(PROTO_PREFS_WHY_BAD, d.why[2]);
    TEST_ASSERT_EQUAL_UINT8(PROTO_PREFS_WHY_BAD, d.why[5]);
    TEST_ASSERT_EQUAL_UINT8(0u, d.why[12]);
    assert_sane(&d.values, d.refused, d.why);
}

/* LINK-050, LINK-051: a newer panel's ids are skipped by their length and named (a PREFS_SET's
 * are refused by id); bytes after the count's last TLV are skipped (appended later). */
static void test_unknown_ids(void)
{
    static const uint8_t IN[] = {
        5u,
        25u, 3u, 0xAAu, 0xBBu, 0xCCu, /* a future field */
        1u, 1u, 40u,                  /* brightness */
        0u, 0u,                       /* id 0 is no field */
        25u, 1u, 7u,                  /* the same future id again: named once */
        200u, 0u,                     /* another, empty */
        0xEEu, 0xFFu,                 /* appended after the list */
    };
    proto_prefs_decoded_t d;
    TEST_ASSERT_TRUE(proto_prefs_decode(IN, sizeof IN, PROTO_PREFS_KNOWN, &d));
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_BIT(1u), d.values.have);
    TEST_ASSERT_EQUAL_UINT8(40u, d.values.brightness);
    TEST_ASSERT_EQUAL_HEX32(0u, d.refused);
    TEST_ASSERT_EQUAL_UINT8(3u, d.unknown_n);
    TEST_ASSERT_EQUAL_UINT8(25u, d.unknown[0]);
    TEST_ASSERT_EQUAL_UINT8(0u, d.unknown[1]);
    TEST_ASSERT_EQUAL_UINT8(200u, d.unknown[2]);

    /* More unknown ids than the list holds: all counted, the first named. */
    uint8_t many[1u + 2u * 12u];
    many[0] = 12u;
    for (unsigned i = 0; i < 12u; i++) {
        many[1u + 2u * i] = (uint8_t)(100u + i);
        many[2u + 2u * i] = 0u;
    }
    TEST_ASSERT_TRUE(proto_prefs_decode(many, sizeof many, PROTO_PREFS_KNOWN, &d));
    TEST_ASSERT_EQUAL_UINT8(12u, d.unknown_n);
    for (unsigned i = 0; i < PROTO_PREFS_UNKNOWN_MAX; i++) {
        TEST_ASSERT_EQUAL_UINT8(100u + i, d.unknown[i]);
    }
    TEST_ASSERT_EQUAL_HEX32(0u, d.values.have);
}

/* LINK-051: each value is range-checked into its own why; a field outside accept is not allowed;
 * a field named twice is refused whole (neither copy applies). */
static void test_out_of_range(void)
{
    static const struct {
        uint8_t id;
        uint32_t value;
        bool ok;
    } C[] = {
        {1u, 9u, false},          {1u, 10u, true},         {1u, 100u, true},
        {1u, 101u, false},        {2u, 0u, true},          {2u, 50u, true},
        {2u, 51u, false},         {3u, 0u, true},          {3u, 1u, false},
        {3u, 9999u, false},       {3u, 10000u, true},      {3u, 86400000u, true},
        {3u, 86400001u, false},   {4u, 5000u, false},      {9u, 0xFFFFFFFFu, false},
        {5u, 1u, true},           {5u, 2u, false},         {6u, 1439u, true},
        {6u, 1440u, false},       {7u, 0xFFFFu, false},    {8u, 9u, false},
        {10u, 2u, true},          {10u, 3u, false},        {11u, 3u, false},
        {12u, 255u, false},       {13u, 0u, true},         {13u, 101u, false},
        {14u, 101u, false},       {15u, 100u, true},       {16u, 2u, false},
        {17u, 2u, false},         {18u, 7u, false},        {19u, 1u, true},
    };
    for (unsigned i = 0; i < sizeof C / sizeof C[0]; i++) {
        const proto_prefs_field_t *f = proto_prefs_field(C[i].id);
        uint8_t in[8] = {1u, C[i].id, f->size};
        for (unsigned b = 0; b < f->size; b++) {
            in[3u + b] = (uint8_t)(C[i].value >> (8u * b));
        }
        proto_prefs_decoded_t d;
        TEST_ASSERT_TRUE(proto_prefs_decode(in, 3u + f->size, PROTO_PREFS_KNOWN, &d));
        const bool taken = (d.values.have & PROTO_PREFS_BIT(C[i].id)) != 0u;
        TEST_ASSERT_EQUAL_MESSAGE(C[i].ok, taken, f->key);
        TEST_ASSERT_EQUAL_UINT8(C[i].ok ? 0u : PROTO_PREFS_WHY_RANGE, d.why[C[i].id - 1u]);
        TEST_ASSERT_EQUAL_INT(C[i].ok ? PROTO_PREFS_WHY_NONE : PROTO_PREFS_WHY_RANGE,
                              proto_prefs_check(C[i].id, C[i].value));
        /* Outside accept: not allowed, whatever the value. */
        TEST_ASSERT_TRUE(proto_prefs_decode(in, 3u + f->size,
                                            PROTO_PREFS_KNOWN & ~PROTO_PREFS_BIT(C[i].id), &d));
        TEST_ASSERT_EQUAL_HEX32(0u, d.values.have);
        TEST_ASSERT_EQUAL_UINT8(PROTO_PREFS_WHY_NOT_ALLOWED, d.why[C[i].id - 1u]);
    }
    static const uint8_t TWICE[] = {3u, 1u, 1u, 40u, 13u, 1u, 20u, 1u, 1u, 50u};
    proto_prefs_decoded_t d;
    TEST_ASSERT_TRUE(proto_prefs_decode(TWICE, sizeof TWICE, PROTO_PREFS_KNOWN, &d));
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_BIT(13u), d.values.have);
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_BIT(1u), d.refused);
    TEST_ASSERT_EQUAL_UINT8(PROTO_PREFS_WHY_BAD, d.why[0]);
    TEST_ASSERT_EQUAL_UINT8(0u, d.values.brightness);
}

/* LINK-050: a TLV that runs past the section refuses the whole list; an empty section is an
 * empty list. */
static void test_truncated(void)
{
    proto_prefs_decoded_t d;
    static const uint8_t LONG_VALUE[] = {1u, 1u, 4u, 80u};
    TEST_ASSERT_FALSE(proto_prefs_decode(LONG_VALUE, sizeof LONG_VALUE, PROTO_PREFS_KNOWN, &d));
    TEST_ASSERT_EQUAL_HEX32(0u, d.values.have);
    static const uint8_t MISSING[] = {2u, 1u, 1u, 80u};
    TEST_ASSERT_FALSE(proto_prefs_decode(MISSING, sizeof MISSING, PROTO_PREFS_KNOWN, &d));
    TEST_ASSERT_EQUAL_HEX32(0u, d.values.have);
    static const uint8_t HALF_HEADER[] = {2u, 1u, 1u, 80u, 13u};
    TEST_ASSERT_FALSE(proto_prefs_decode(HALF_HEADER, sizeof HALF_HEADER, PROTO_PREFS_KNOWN, &d));
    TEST_ASSERT_TRUE(proto_prefs_decode(NULL, 0u, PROTO_PREFS_KNOWN, &d));
    TEST_ASSERT_EQUAL_HEX32(0u, d.values.have);
    static const uint8_t EMPTY[] = {0u};
    TEST_ASSERT_TRUE(proto_prefs_decode(EMPTY, sizeof EMPTY, PROTO_PREFS_KNOWN, &d));
    /* Every cut of the golden list is refused: its count promises more. */
    for (size_t n = 1u; n < sizeof GOLDEN; n++) {
        TEST_ASSERT_FALSE(proto_prefs_decode(GOLDEN, n, PROTO_PREFS_KNOWN, &d));
    }
}

/* LINK-051: put checks before it stores; get reads only what is held. */
static void test_put_get(void)
{
    proto_prefs_values_t v;
    memset(&v, 0, sizeof v);
    uint32_t x = 7u;
    TEST_ASSERT_FALSE(proto_prefs_get(&v, PROTO_PREFS_F_BRIGHTNESS, &x));
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_RANGE, proto_prefs_put(&v, PROTO_PREFS_F_BRIGHTNESS, 5u));
    TEST_ASSERT_EQUAL_HEX32(0u, v.have);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_NONE, proto_prefs_put(&v, PROTO_PREFS_F_BRIGHTNESS, 55u));
    TEST_ASSERT_TRUE(proto_prefs_get(&v, PROTO_PREFS_F_BRIGHTNESS, &x));
    TEST_ASSERT_EQUAL_UINT32(55u, x);
    TEST_ASSERT_EQUAL_UINT8(55u, v.brightness);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_NONE,
                          proto_prefs_put(&v, PROTO_PREFS_F_NIGHT_OFF_AFTER, 3600000u));
    TEST_ASSERT_EQUAL_UINT32(3600000u, v.night_off_after_ms);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_NONE,
                          proto_prefs_put(&v, PROTO_PREFS_F_NIGHT_END, 1439u));
    TEST_ASSERT_EQUAL_UINT16(1439u, v.night_end);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_NONE, proto_prefs_put(&v, PROTO_PREFS_F_VOICE_CHIME, 1u));
    TEST_ASSERT_TRUE(v.voice_chime);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_NOT_ALLOWED, proto_prefs_put(&v, 0u, 1u));
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_NOT_ALLOWED, proto_prefs_put(&v, 25u, 1u));
    TEST_ASSERT_FALSE(proto_prefs_get(&v, 25u, &x));
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_BIT(1u) | PROTO_PREFS_BIT(9u) | PROTO_PREFS_BIT(7u) |
                                PROTO_PREFS_BIT(19u),
                            v.have);
}

static const char *write_values(char *buf, size_t cap, const proto_prefs_values_t *v)
{
    pj_writer_t w;
    pj_init(&w, buf, cap);
    proto_prefs_json_values(&w, v);
    TEST_ASSERT_TRUE(pj_finish(&w) > 0u);
    return buf;
}

/* A settings_set's values object on its own. */
static bool parse(const char *json, proto_prefs_mask_t accept, proto_prefs_parsed_t *out)
{
    pj_reader_t r;
    pj_read_init(&r, json, strlen(json));
    const bool ok = proto_prefs_json_parse(&r, accept, out);
    return ok && pj_finished(&r);
}

/* A values value refused whole (settings_result's error "bad") that is still good JSON: nothing
 * taken or refused, and the reader is past it, not in error. */
static void expect_whole_bad(const char *json)
{
    pj_reader_t r;
    pj_read_init(&r, json, strlen(json));
    proto_prefs_parsed_t p;
    memset(&p, 0xEE, sizeof p);
    TEST_ASSERT_FALSE_MESSAGE(proto_prefs_json_parse(&r, PROTO_PREFS_KNOWN, &p), json);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0u, p.values.have, json);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0u, p.refused, json);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, p.unknown_n, json);
    for (unsigned id = 1u; id <= PROTO_PREFS_F_LAST; id++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, p.why[id - 1u], json);
    }
    TEST_ASSERT_FALSE_MESSAGE(r.error, json);
    TEST_ASSERT_TRUE_MESSAGE(pj_finished(&r), json);
}

/* HAI-017: the values object (delays in seconds, switches true / false) and the can array are
 * golden; the longest fits PROTO_PREFS_JSON_MAX. */
static void test_json_values(void)
{
    char buf[512];
    const proto_prefs_values_t v = defaults();
    TEST_ASSERT_EQUAL_STRING(GOLDEN_JSON, write_values(buf, sizeof buf, &v));

    proto_prefs_values_t none;
    memset(&none, 0, sizeof none);
    TEST_ASSERT_EQUAL_STRING("{}", write_values(buf, sizeof buf, &none));

    proto_prefs_values_t some;
    memset(&some, 0, sizeof some);
    proto_prefs_put(&some, PROTO_PREFS_F_OFF_AFTER, 300000u);
    proto_prefs_put(&some, PROTO_PREFS_F_HIGH_CONTRAST, 1u);
    proto_prefs_put(&some, PROTO_PREFS_F_BRIGHTNESS, 40u);
    TEST_ASSERT_EQUAL_STRING("{\"brightness\":40,\"off_after\":300,\"contrast\":true}",
                             write_values(buf, sizeof buf, &some));

    /* The longest values object, NUL included. */
    proto_prefs_values_t big;
    memset(&big, 0, sizeof big);
    for (unsigned id = 1u; id <= PROTO_PREFS_F_LAST; id++) {
        const proto_prefs_field_t *f = proto_prefs_field(id);
        const uint32_t longest = (f->flags & PROTO_PREFS_FL_SWITCH) != 0u ? 0u : f->max;
        TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_NONE, proto_prefs_put(&big, id, longest));
    }
    char exact[PROTO_PREFS_JSON_MAX];
    pj_writer_t w;
    pj_init(&w, exact, sizeof exact);
    proto_prefs_json_values(&w, &big);
    const size_t n = pj_finish(&w);
    TEST_ASSERT_EQUAL_UINT(316u, n);
    TEST_ASSERT_TRUE(n + 1u <= PROTO_PREFS_JSON_MAX);

    pj_init(&w, buf, sizeof buf);
    proto_prefs_json_keys(&w, PROTO_PREFS_BIT(1u) | PROTO_PREFS_BIT(17u) | PROTO_PREFS_BIT(9u) |
                                  0x80000000u);
    TEST_ASSERT_TRUE(pj_finish(&w) > 0u);
    TEST_ASSERT_EQUAL_STRING("[\"brightness\",\"night_off\",\"voice\"]", buf);
    pj_init(&w, buf, sizeof buf);
    proto_prefs_json_keys(&w, 0u);
    TEST_ASSERT_TRUE(pj_finish(&w) > 0u);
    TEST_ASSERT_EQUAL_STRING("[]", buf);
    pj_init(&w, buf, sizeof buf);
    proto_prefs_json_keys(&w, PROTO_PREFS_KNOWN);
    TEST_ASSERT_EQUAL_UINT(225u, pj_finish(&w));
}

/* HAI-017: values written are parsed back the same; random sets too (delays in whole seconds). */
static void test_json_round_trip(void)
{
    char buf[PROTO_PREFS_JSON_MAX];
    proto_prefs_parsed_t p;
    const proto_prefs_values_t v = defaults();
    TEST_ASSERT_TRUE(parse(write_values(buf, sizeof buf, &v), PROTO_PREFS_KNOWN, &p));
    assert_same_values(&v, &p.values);
    TEST_ASSERT_EQUAL_HEX32(0u, p.refused);
    TEST_ASSERT_EQUAL_UINT8(0u, p.unknown_n);
    s_seed = 0x15u;
    for (unsigned iter = 0; iter < 3000u; iter++) {
        const proto_prefs_values_t r = random_values(rnd() & PROTO_PREFS_KNOWN, true);
        TEST_ASSERT_TRUE(parse(write_values(buf, sizeof buf, &r), PROTO_PREFS_KNOWN, &p));
        assert_same_values(&r, &p.values);
        TEST_ASSERT_EQUAL_HEX32(0u, p.refused);
    }
}

/* HAI-017: a settings_set's values - each key taken or refused alone (out of range, not allowed);
 * an unknown key is not allowed by name; a value that isn't of its key's kind, a key named twice,
 * an empty key or one longer than any key, a values that isn't an object and a damaged object
 * refuse the whole request (error "bad": "refused" has no word for them). */
static void test_json_parse(void)
{
    proto_prefs_parsed_t p;
    TEST_ASSERT_TRUE(parse("{\"brightness\":40}", PROTO_PREFS_KNOWN, &p));
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_BIT(1u), p.values.have);
    TEST_ASSERT_EQUAL_UINT8(40u, p.values.brightness);

    TEST_ASSERT_TRUE(parse("{\"night_off\":600,\"saver_after\":0,\"off_after\":86400,"
                           "\"key_click\":false,\"night\":true}",
                           PROTO_PREFS_KNOWN, &p));
    TEST_ASSERT_EQUAL_UINT32(600000u, p.values.night_off_after_ms);
    TEST_ASSERT_EQUAL_UINT32(0u, p.values.saver_after_ms);
    TEST_ASSERT_EQUAL_UINT32(86400000u, p.values.off_after_ms);
    TEST_ASSERT_FALSE(p.values.key_click);
    TEST_ASSERT_TRUE(p.values.night_on);
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_BIT(3u) | PROTO_PREFS_BIT(4u) | PROTO_PREFS_BIT(5u) |
                                PROTO_PREFS_BIT(9u) | PROTO_PREFS_BIT(16u),
                            p.values.have);

    /* Each refused alone, with its why. */
    TEST_ASSERT_TRUE(parse("{\"brightness\":101,\"beeps\":30,\"saver_after\":5,\"off_after\":86401,"
                           "\"night_off\":-1,\"appearance\":3,\"voice_vol\":4294967296}",
                           PROTO_PREFS_KNOWN, &p));
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_BIT(13u), p.values.have);
    TEST_ASSERT_EQUAL_UINT8(30u, p.values.volume);
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_BIT(1u) | PROTO_PREFS_BIT(3u) | PROTO_PREFS_BIT(4u) |
                                PROTO_PREFS_BIT(9u) | PROTO_PREFS_BIT(11u) | PROTO_PREFS_BIT(15u),
                            p.refused);
    TEST_ASSERT_EQUAL_UINT8(PROTO_PREFS_WHY_RANGE, p.why[0]);  /* brightness 101 */
    TEST_ASSERT_EQUAL_UINT8(PROTO_PREFS_WHY_RANGE, p.why[2]);  /* 5 s */
    TEST_ASSERT_EQUAL_UINT8(PROTO_PREFS_WHY_RANGE, p.why[3]);  /* a day and a second */
    TEST_ASSERT_EQUAL_UINT8(PROTO_PREFS_WHY_RANGE, p.why[8]);  /* negative */
    TEST_ASSERT_EQUAL_UINT8(PROTO_PREFS_WHY_RANGE, p.why[10]); /* appearance 3 */
    TEST_ASSERT_EQUAL_UINT8(PROTO_PREFS_WHY_RANGE, p.why[14]); /* beyond u32 */
    assert_sane(&p.values, p.refused, p.why);

    /* A value not of its key's kind, or a key named twice: the whole request is bad, never one
     * field refused BAD, and the reader is past the object. */
    static const char *const WHOLE[] = {
        "{\"beeps\":30,\"night\":1}",                            /* a switch as a number */
        "{\"beeps\":30,\"contrast\":\"on\"}",                    /* a switch as a string */
        "{\"beeps\":30,\"night_start\":60.5}",                   /* a fraction */
        "{\"beeps\":30,\"clock\":null}",                         /* null */
        "{\"beeps\":30,\"chime_vol\":99999999999999999999}",     /* beyond int64 */
        "{\"beeps\":30,\"saver_after\":[60]}",                   /* an array */
        "{\"beeps\":30,\"night_end\":{\"h\":7}}",                /* an object */
        "{\"voice\":true,\"beeps\":30,\"voice\":true}",          /* named twice */
        "{\"beeps\":30,\"beeps\":30}",
        "{\"beeps\":30,\"beeps\":\"x\"}",
        "{\"\":1}", "{\"brightness_x\":1}",                      /* 12 bytes: longer than any key */
        "{\"brightness\":40,\"a_much_longer_key\":1}",
        "[]", "40", "\"brightness\"", "null", "true",            /* values isn't an object */
    };
    for (unsigned i = 0; i < sizeof WHOLE / sizeof WHOLE[0]; i++) {
        expect_whole_bad(WHOLE[i]);
    }

    /* Unknown keys: refused not_allowed by name, the others taken; nested values skipped. */
    TEST_ASSERT_TRUE(parse("{\"lang\":\"fr\",\"brightness\":50,\"future_x\":{\"a\":[1,2]},"
                           "\"lang\":\"de\",\"region\":\"FR\"}",
                           PROTO_PREFS_KNOWN, &p));
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_BIT(1u), p.values.have);
    TEST_ASSERT_EQUAL_HEX32(0u, p.refused);
    TEST_ASSERT_EQUAL_UINT8(3u, p.unknown_n);
    TEST_ASSERT_EQUAL_STRING("lang", p.unknown[0]);
    TEST_ASSERT_EQUAL_STRING("future_x", p.unknown[1]);
    TEST_ASSERT_EQUAL_STRING("region", p.unknown[2]);
    TEST_ASSERT_TRUE(parse("{\"a\":1,\"b\":1,\"c\":1,\"d\":1,\"e\":1,\"f\":1,\"g\":1,\"h\":1,"
                           "\"i\":1,\"j\":1}",
                           PROTO_PREFS_KNOWN, &p));
    TEST_ASSERT_EQUAL_UINT8(10u, p.unknown_n);
    TEST_ASSERT_EQUAL_STRING("h", p.unknown[PROTO_PREFS_UNKNOWN_MAX - 1u]);

    /* Outside what the target takes: not allowed. */
    TEST_ASSERT_TRUE(parse("{\"voice\":true,\"beeps\":10}",
                           PROTO_PREFS_KNOWN & ~PROTO_PREFS_BIT(17u), &p));
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_BIT(13u), p.values.have);
    TEST_ASSERT_EQUAL_UINT8(PROTO_PREFS_WHY_NOT_ALLOWED, p.why[16]);

    /* Damaged JSON: the whole request is bad too. */
    static const char *const BAD[] = {
        "{\"brightness\":40", "{\"brightness\":40,}", "{\"brightness\":4x}", "", "x",
    };
    for (unsigned i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        TEST_ASSERT_FALSE_MESSAGE(parse(BAD[i], PROTO_PREFS_KNOWN, &p), BAD[i]);
        TEST_ASSERT_EQUAL_HEX32(0u, p.values.have);
    }
    /* An empty object changes nothing. */
    TEST_ASSERT_TRUE(parse("{ }", PROTO_PREFS_KNOWN, &p));
    TEST_ASSERT_EQUAL_HEX32(0u, p.values.have);
    TEST_ASSERT_EQUAL_UINT8(0u, p.unknown_n);
}

/* A settings_set message read as ha_link reads it: its id and target, and whether its values were
 * taken (*got). true when the whole message was read. */
static bool read_set(const char *msg, char id[16], char target[8], bool *got,
                     proto_prefs_parsed_t *p)
{
    pj_reader_t r;
    pj_read_init(&r, msg, strlen(msg));
    TEST_ASSERT_TRUE(pj_enter_obj(&r));
    char key[16];
    id[0] = '\0';
    target[0] = '\0';
    *got = false;
    while (pj_next_member(&r, key, sizeof key)) {
        if (strcmp(key, "values") == 0) {
            *got = proto_prefs_json_parse(&r, PROTO_PREFS_KNOWN, p);
        } else if (strcmp(key, "id") == 0) {
            TEST_ASSERT_TRUE(pj_get_str(&r, id, 16u));
        } else if (strcmp(key, "target") == 0) {
            TEST_ASSERT_TRUE(pj_get_str(&r, target, 8u));
        }
    }
    return pj_finished(&r);
}

/* HAI-017: inside a settings_set frame, the reader goes on after the values object; and after a
 * values refused whole that is still good JSON (not an object, a value of the wrong kind), so the
 * answer still names the request's id and target. */
static void test_json_in_message(void)
{
    char id[16], target[8];
    bool got = false;
    proto_prefs_parsed_t p;
    TEST_ASSERT_TRUE(read_set("{\"t\":\"settings_set\",\"id\":\"3f9a1c2e\",\"values\":{\"beeps\":0,"
                              "\"night_start\":1380},\"target\":\"a1b2c3\"}",
                              id, target, &got, &p));
    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL_STRING("3f9a1c2e", id);
    TEST_ASSERT_EQUAL_STRING("a1b2c3", target);
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_BIT(13u) | PROTO_PREFS_BIT(6u), p.values.have);
    TEST_ASSERT_EQUAL_UINT8(0u, p.values.volume);
    TEST_ASSERT_EQUAL_UINT16(1380u, p.values.night_start);

    static const char *const WHOLE[] = {
        "{\"t\":\"settings_set\",\"values\":[1,{\"a\":2}],\"id\":\"3f9a1c2e\",\"target\":\"a1b2c3\"}",
        "{\"t\":\"settings_set\",\"values\":\"brightness\",\"id\":\"3f9a1c2e\",\"target\":\"a1b2c3\"}",
        "{\"t\":\"settings_set\",\"values\":null,\"id\":\"3f9a1c2e\",\"target\":\"a1b2c3\"}",
        "{\"t\":\"settings_set\",\"values\":{\"night\":1,\"beeps\":2},\"id\":\"3f9a1c2e\","
        "\"target\":\"a1b2c3\"}",
        "{\"t\":\"settings_set\",\"values\":{\"beeps\":1,\"beeps\":2},\"id\":\"3f9a1c2e\","
        "\"target\":\"a1b2c3\"}",
    };
    for (unsigned i = 0; i < sizeof WHOLE / sizeof WHOLE[0]; i++) {
        TEST_ASSERT_TRUE_MESSAGE(read_set(WHOLE[i], id, target, &got, &p), WHOLE[i]);
        TEST_ASSERT_FALSE_MESSAGE(got, WHOLE[i]);
        TEST_ASSERT_EQUAL_HEX32(0u, p.values.have);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("3f9a1c2e", id, WHOLE[i]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("a1b2c3", target, WHOLE[i]);
    }
}

/* LINK-050, LINK-051: random and mutated lists never read out of bounds (each ends at the end of
 * its buffer, so ASan sees a read past it); whatever is taken is in range and re-encodes to what
 * decodes the same. */
static void test_fuzz_decode(void)
{
    static uint8_t arena[96];
    s_seed = 0xF22u;
    uint8_t buf[96];
    for (unsigned iter = 0; iter < 60000u; iter++) {
        size_t len;
        if (iter % 2u == 0u) {
            len = rnd() % sizeof buf;
            for (size_t i = 0; i < len; i++) {
                const uint32_t r = rnd();
                /* Mostly small ids and lengths, so fields are often hit. */
                buf[i] = (uint8_t)(r % 3u == 0u ? r >> 8 : (r >> 8) % 24u);
            }
        } else {
            memcpy(buf, GOLDEN, sizeof GOLDEN);
            len = sizeof GOLDEN;
            const unsigned flips = 1u + rnd() % 4u;
            for (unsigned k = 0; k < flips; k++) {
                buf[rnd() % len] ^= (uint8_t)(1u << (rnd() % 8u));
            }
            len -= rnd() % 3u;
        }
        uint8_t *in = arena + sizeof arena - len;
        memcpy(in, buf, len);
        proto_prefs_decoded_t d;
        const proto_prefs_mask_t accept = iter % 3u == 0u ? rnd() : PROTO_PREFS_KNOWN;
        if (!proto_prefs_decode(in, len, accept, &d)) {
            TEST_ASSERT_EQUAL_HEX32(0u, d.values.have);
            continue;
        }
        assert_sane(&d.values, d.refused, d.why);
        TEST_ASSERT_EQUAL_HEX32(0u, d.values.have & ~accept);
        for (unsigned id = 1u; id <= PROTO_PREFS_F_LAST; id++) {
            const uint8_t w = d.why[id - 1u];
            TEST_ASSERT_TRUE(w == 0u || w == PROTO_PREFS_WHY_BAD || w == PROTO_PREFS_WHY_RANGE ||
                             w == PROTO_PREFS_WHY_NOT_ALLOWED);
        }
        uint8_t again[PROTO_PREFS_ENC_MAX];
        const size_t n = proto_prefs_encode(&d.values, PROTO_PREFS_KNOWN, again, sizeof again);
        TEST_ASSERT_TRUE(n >= 1u);
        proto_prefs_decoded_t e;
        TEST_ASSERT_TRUE(proto_prefs_decode(again, n, PROTO_PREFS_KNOWN, &e));
        assert_same_values(&d.values, &e.values);
    }
}

/* HAI-017: random values objects never read out of bounds (each ends at the end of its buffer);
 * whatever is taken is in range, and no field is ever refused BAD. */
static void test_fuzz_json(void)
{
    static char arena[256];
    static const char *const PART[] = {
        "{", "}", "[", "]", ",", ":", "\"", "brightness", "voice", "night_off", "contrast",
        "saver_after", "beeps", "x", "1", "40", "-3", "86400", "1e3", "0.5", "true", "false",
        "null", " ", "\\u0000", "\\\"", "99999999999",
    };
    s_seed = 0x750Bu;
    char buf[256];
    for (unsigned iter = 0; iter < 40000u; iter++) {
        size_t len = 0u;
        if (iter % 2u == 0u) {
            /* A plausible object: {"key":value,...} with random keys and values. */
            buf[len++] = '{';
            const unsigned members = rnd() % 6u;
            for (unsigned m = 0; m < members && len < 200u; m++) {
                const char *k = PART[7u + rnd() % 7u];
                const char *val = PART[14u + rnd() % 13u];
                len += (size_t)snprintf(buf + len, sizeof buf - len, "%s\"%s\":%s",
                                        m > 0u ? "," : "", k, val);
            }
            buf[len++] = '}';
        } else {
            const unsigned parts = rnd() % 24u;
            for (unsigned k = 0; k < parts; k++) {
                const char *s = PART[rnd() % (sizeof PART / sizeof PART[0])];
                const size_t sl = strlen(s);
                if (len + sl >= sizeof buf) {
                    break;
                }
                memcpy(buf + len, s, sl);
                len += sl;
            }
        }
        char *in = arena + sizeof arena - len;
        memcpy(in, buf, len);
        pj_reader_t r;
        pj_read_init(&r, in, len);
        proto_prefs_parsed_t p;
        const proto_prefs_mask_t accept = iter % 3u == 0u ? rnd() : PROTO_PREFS_KNOWN;
        if (!proto_prefs_json_parse(&r, accept, &p)) {
            TEST_ASSERT_EQUAL_HEX32(0u, p.values.have);
            TEST_ASSERT_EQUAL_HEX32(0u, p.refused);
            continue;
        }
        assert_sane(&p.values, p.refused, p.why);
        TEST_ASSERT_EQUAL_HEX32(0u, p.values.have & ~accept);
        for (unsigned id = 1u; id <= PROTO_PREFS_F_LAST; id++) {
            TEST_ASSERT_TRUE(p.why[id - 1u] != PROTO_PREFS_WHY_BAD);
        }
        for (unsigned u = 0; u < p.unknown_n && u < PROTO_PREFS_UNKNOWN_MAX; u++) {
            const size_t kl = strlen(p.unknown[u]);
            TEST_ASSERT_TRUE(kl >= 1u && kl <= PROTO_PREFS_KEY_MAX);
            TEST_ASSERT_EQUAL_UINT8(0u, proto_prefs_id(p.unknown[u]));
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_frozen_table);
    RUN_TEST(test_why);
    RUN_TEST(test_golden_and_crc);
    RUN_TEST(test_tlv_round_trip);
    RUN_TEST(test_wrong_length);
    RUN_TEST(test_unknown_ids);
    RUN_TEST(test_out_of_range);
    RUN_TEST(test_truncated);
    RUN_TEST(test_put_get);
    RUN_TEST(test_json_values);
    RUN_TEST(test_json_round_trip);
    RUN_TEST(test_json_parse);
    RUN_TEST(test_json_in_message);
    RUN_TEST(test_fuzz_decode);
    RUN_TEST(test_fuzz_json);
    return UNITY_END();
}
