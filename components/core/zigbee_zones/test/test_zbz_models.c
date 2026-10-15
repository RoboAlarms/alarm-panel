/*
 * test_zbz_models.c - the model table, checked whole (docs/features/20 devices.md §10,
 * testing.md §5; ZB-007, ZB-023, ZB-024): every seeded signature resolves to its expected row,
 * no row is hidden behind an earlier one, near misses don't match, every row's words are
 * non-empty, nothing is brand-only, and the siren and keypad flags sit only where they mean
 * something.
 */
#include <stdint.h>
#include <string.h>

#include "signatures/zb_signatures.h"
#include "unity.h"
#include "zigbee_zones/zbz_models.h"
#include "zigbee_zones/zbz_registry.h"

void setUp(void) {}
void tearDown(void) {}

static bool is_prefix(const char *pattern, const char *text)
{
    if (pattern == NULL || pattern[0] == '\0') {
        return true;
    }
    return text != NULL && strncmp(pattern, text, strlen(pattern)) == 0;
}

static bool empty(const char *s)
{
    return s == NULL || s[0] == '\0';
}

static bool same_ci(const char *a, const char *b)
{
    for (size_t k = 0;; k++) {
        unsigned ca = (unsigned)(uint8_t)a[k], cb = (unsigned)(uint8_t)b[k];
        ca = (ca >= 'A' && ca <= 'Z') ? ca + 32u : ca;
        cb = (cb >= 'A' && cb <= 'Z') ? cb + 32u : cb;
        if (ca != cb) {
            return false;
        }
        if (ca == 0u) {
            return true;
        }
    }
}

/* ZB-007: no row is unreachable behind an earlier one - an earlier row whose manufacturer and
 * model patterns are both prefixes of a later row's would always win first. */
static void test_no_row_hidden_by_an_earlier_one(void)
{
    size_t n = 0;
    const zbz_model_t *t = zbz_model_table(&n);
    TEST_ASSERT_TRUE(n > 0u);
    for (size_t j = 0; j < n; j++) {
        for (size_t i = 0; i < j; i++) {
            const bool hides = is_prefix(t[i].manufacturer, empty(t[j].manufacturer)
                                                                ? ""
                                                                : t[j].manufacturer) &&
                               (empty(t[i].manufacturer) || !empty(t[j].manufacturer)) &&
                               is_prefix(t[i].model, t[j].model);
            TEST_ASSERT_FALSE_MESSAGE(hides, t[j].model);
        }
        /* and each row finds itself when asked with its own patterns */
        TEST_ASSERT_EQUAL_PTR(&t[j], zbz_model_for(empty(t[j].manufacturer) ? "anyone"
                                                                             : t[j].manufacturer,
                                                   t[j].model));
    }
}

/* ZB-007: every row's words are there, nothing is brand-only (each row names a model, and no
 * name is a brand), and the flags fit the row's role. */
static void test_every_row_is_well_formed(void)
{
    static const char *const BRANDS[] = {
        "SONOFF", "LUMI", "Aqara", "Xiaomi", "IKEA", "IKEA of Sweden", "HEIMAN", "frient",
        "frient A/S", "Develco", "Develco Products A/S", "CentraLite", "Tuya", "Third Reality",
        "Xfinity", "Universal Electronics Inc", "Linkind",
    };
    const uint16_t wd_flags = ZBZ_MODEL_WD_REVERSED | ZBZ_MODEL_WD_ONE_TONE | ZBZ_MODEL_WD_NO_LEVEL;
    const uint16_t ace_flags = ZBZ_MODEL_ACE_ARMED_80 | ZBZ_MODEL_ACE_TOUCH_WAKE |
                               ZBZ_MODEL_ACE_AUDIBLE | ZBZ_MODEL_ACE_FOB | ZBZ_MODEL_ACE_INIT;
    const uint16_t known = (uint16_t)(wd_flags | ace_flags | ZBZ_MODEL_NO_DEFAULT_RSP);
    size_t n = 0;
    const zbz_model_t *t = zbz_model_table(&n);
    for (size_t i = 0; i < n; i++) {
        const zbz_model_t *m = &t[i];
        TEST_ASSERT_FALSE_MESSAGE(empty(m->model), m->name);           /* never brand-only */
        TEST_ASSERT_FALSE(empty(m->name));
        TEST_ASSERT_TRUE(strlen(m->name) < 40u);                       /* UI_ZB_MODEL_LEN */
        TEST_ASSERT_TRUE(m->add_words == NULL || (m->add_words[0] != '\0' &&
                                                  strlen(m->add_words) < 64u));
        TEST_ASSERT_TRUE(m->wake_words == NULL || (m->wake_words[0] != '\0' &&
                                                   strlen(m->wake_words) < 64u));
        for (size_t b = 0; b < sizeof(BRANDS) / sizeof(BRANDS[0]); b++) {
            TEST_ASSERT_FALSE_MESSAGE(same_ci(m->name, BRANDS[b]), m->name);
        }
        TEST_ASSERT_FALSE(!empty(m->manufacturer) && same_ci(m->name, m->manufacturer));
        TEST_ASSERT_TRUE(m->role <= (uint8_t)ZBZ_ROLE_OTHER);
        TEST_ASSERT_EQUAL_UINT16(0u, (uint16_t)(m->flags & (uint16_t)~known));
        if ((m->flags & wd_flags) != 0u) {
            TEST_ASSERT_EQUAL_UINT8(ZBZ_ROLE_SIREN, m->role);
        }
        if ((m->flags & ace_flags) != 0u) {
            TEST_ASSERT_TRUE(m->role == (uint8_t)ZBZ_ROLE_KEYPAD ||
                             m->role == (uint8_t)ZBZ_ROLE_KEYFOB);
        }
        if ((m->flags & ZBZ_MODEL_ACE_FOB) != 0u) {
            TEST_ASSERT_EQUAL_UINT8(ZBZ_ROLE_KEYFOB, m->role);
        }
        TEST_ASSERT_TRUE(m->max_duration_s == 0u || m->max_duration_s >= 10u);
    }
}

/* ZB-007, ZB-023, ZB-024: each seeded signature resolves to its row, and a row's role, when it
 * gives one, is the role the signature expects (testing.md §2). */
static void test_signatures_resolve_to_their_rows(void)
{
    static const struct {
        const char *file;
        const char *name; /* NULL: no row (decided by its clusters) */
    } EXPECT[] = {
        {"aqara_magnet_agl02.json", "Door sensor"},
        {"aqara_motion_agl04.json", "Motion sensor"},
        {"centralite_3300.json", "Door sensor"},
        {"centralite_3326l.json", "Motion sensor"},
        {"centralite_3405l.json", "Keypad"},
        {"frient_kepzb_110.json", "Keypad"},
        {"frient_sirzb_111.json", "Siren"},
        {"heiman_hs1rc.json", "Key fob"},
        {"heiman_smoke_em.json", "Smoke alarm"},
        {"ikea_parasoll.json", "Door sensor"},
        {"sonoff_snzb_02d.json", "Temperature sensor"},
        {"sonoff_snzb_04pr2.json", "Door sensor"},
        {"third_reality_3rds17bz.json", "Door sensor"},
        {"tuya_ts0216.json", "Siren"},
        {"tuya_ts0601_siren.json", NULL},
        {"xfinity_urc4450bc0.json", "Keypad"},
    };
    const size_t n = sizeof(ZB_SIGNATURES) / sizeof(ZB_SIGNATURES[0]);
    TEST_ASSERT_EQUAL_size_t(sizeof(EXPECT) / sizeof(EXPECT[0]), n);
    for (size_t i = 0; i < n; i++) {
        const zbsig_t *s = &ZB_SIGNATURES[i];
        TEST_ASSERT_EQUAL_STRING(EXPECT[i].file, s->file);
        const zbz_model_t *m = zbz_model_for(s->manufacturer, s->model);
        if (EXPECT[i].name == NULL) {
            TEST_ASSERT_NULL_MESSAGE(m, s->file);
            continue;
        }
        TEST_ASSERT_NOT_NULL_MESSAGE(m, s->file);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(EXPECT[i].name, m->name, s->file);
        if (m->role != 0u) {
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(s->role, m->role, s->file);
        }
    }
    /* ZB-023: the one-tone siren under both of its names; ZB-024: the keypad flags. */
    TEST_ASSERT_TRUE((zbz_model_for("_TYZB01_8scntis1", "TS0216")->flags &
                      ZBZ_MODEL_WD_ONE_TONE) != 0u);
    TEST_ASSERT_TRUE((zbz_model_for("HEIMAN", "WarningDevice-EF-3.0")->flags &
                      ZBZ_MODEL_WD_ONE_TONE) != 0u);
    TEST_ASSERT_TRUE((zbz_model_for("CentraLite", "3400-D")->flags & ZBZ_MODEL_ACE_ARMED_80) != 0u);
    TEST_ASSERT_EQUAL_UINT16(0u, zbz_model_for("CentraLite", "3400")->flags);
    TEST_ASSERT_TRUE((zbz_model_for("frient A/S", "KEPZB-110")->flags & ZBZ_MODEL_ACE_AUDIBLE) != 0u);
}

/* ZB-007: near misses don't match - case, a sibling family, a shorter model, nothing at all. */
static void test_near_misses_are_refused(void)
{
    TEST_ASSERT_NULL(zbz_model_for("lumi", "lumi.magnet.agl02"));     /* case matters */
    TEST_ASSERT_NULL(zbz_model_for("LUMI", "lumi.magnet.agl0"));      /* a shorter model */
    TEST_ASSERT_NULL(zbz_model_for("LUMI", "lumi.sensor_magnet"));    /* the older contact */
    TEST_ASSERT_NULL(zbz_model_for("_TZE200_d0yu2xgi", "TS0601"));   /* clusters decide it */
    TEST_ASSERT_NULL(zbz_model_for("_TZ3000_abc", "TS021"));
    TEST_ASSERT_NULL(zbz_model_for("CentraLite", "330"));
    TEST_ASSERT_NULL(zbz_model_for("Centralite", "3300"));
    TEST_ASSERT_NULL(zbz_model_for(NULL, NULL));
    TEST_ASSERT_NULL(zbz_model_for("", ""));
    TEST_ASSERT_NULL(zbz_model_for("SONOFF", NULL));
    TEST_ASSERT_NULL(zbz_model_for(NULL, "SNZB-04P"));                /* a manufacturer row */
    TEST_ASSERT_NOT_NULL(zbz_model_for(NULL, "TS0216"));             /* no manufacturer needed */
}

/* ZB-007: a model at the 32-byte buffers' limit (31 characters and its NUL) still matches by
 * prefix, and the same text cut one character shorter does too. */
static void test_long_texts_still_match(void)
{
    char model[ZBZ_TEXT_LEN];
    memset(model, 'x', sizeof(model));
    memcpy(model, "SNZB-04PR2", 10u);
    model[ZBZ_TEXT_LEN - 1u] = '\0';
    TEST_ASSERT_EQUAL_STRING("Door sensor", zbz_model_for("SONOFF", model)->name);
    model[ZBZ_TEXT_LEN - 2u] = '\0';
    TEST_ASSERT_EQUAL_STRING("Door sensor", zbz_model_for("SONOFF", model)->name);
    char mfr[ZBZ_TEXT_LEN];
    memset(mfr, 'y', sizeof(mfr));
    memcpy(mfr, "Third Reality", 13u);
    mfr[ZBZ_TEXT_LEN - 1u] = '\0';
    TEST_ASSERT_EQUAL_STRING("Door sensor", zbz_model_for(mfr, "3RDS17BZ")->name);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_no_row_hidden_by_an_earlier_one);
    RUN_TEST(test_every_row_is_well_formed);
    RUN_TEST(test_signatures_resolve_to_their_rows);
    RUN_TEST(test_near_misses_are_refused);
    RUN_TEST(test_long_texts_still_match);
    return UNITY_END();
}
