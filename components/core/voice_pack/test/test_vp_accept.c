/*
 * test_vp_accept.c - what the panel accepts of a voice pack and its immutable identity
 * (docs/features/08 voice-first-build.md D08, D12). AUD-024 (one locale and voice), AUD-027
 * (install and boot refuse a DRAFT pack on a release build, and different audio reusing a
 * pack's identity).
 */
#include <string.h>

#include "unity.h"
#include "voice_pack/vp_accept.h"
#include "voice_pack_fixture_v1.h"

void setUp(void);
void tearDown(void);
void setUp(void) {}
void tearDown(void) {}

static vp_pack_t good(void)
{
    vp_pack_t p;
    memset(&p, 0, sizeof(p));
    strcpy(p.locale, "en-US");
    strcpy(p.voice_id, "qwen3-designed_female");
    p.pack_revision = 1u;
    return p;
}

static void digest(uint8_t d[VP_DIGEST_SIZE], uint8_t seed)
{
    for (unsigned i = 0u; i < VP_DIGEST_SIZE; i++) {
        d[i] = (uint8_t)(seed + i);
    }
}

/* AUD-024: the frozen fixture is the one voice; it is accepted on dev and release builds. */
static void test_accept_fixture(void)
{
    vp_pack_t p;
    TEST_ASSERT_EQUAL_INT(VP_OK, vp_open(voice_pack_fixture_v1, sizeof(voice_pack_fixture_v1), &p, NULL));
    TEST_ASSERT_EQUAL_INT(VP_ACCEPT_OK, vp_accept(&p, false));
    TEST_ASSERT_EQUAL_INT(VP_ACCEPT_OK, vp_accept(&p, true));
}

/* AUD-027: DRAFT is taken by a dev build and refused by a release build. */
static void test_accept_draft(void)
{
    vp_pack_t p = good();
    p.flags = VP_FLAG_DRAFT;
    TEST_ASSERT_EQUAL_INT(VP_ACCEPT_OK, vp_accept(&p, false));
    TEST_ASSERT_EQUAL_INT(VP_ACCEPT_DRAFT_RELEASE, vp_accept(&p, true));
    TEST_ASSERT_EQUAL_STRING("draft", vp_accept_name(VP_ACCEPT_DRAFT_RELEASE));
}

/* AUD-024: another locale or voice is refused, reader-valid or not; a prefix is not the voice. */
static void test_accept_other_voice(void)
{
    vp_pack_t p = good();
    strcpy(p.locale, "en-GB");
    TEST_ASSERT_EQUAL_INT(VP_ACCEPT_LOCALE_OTHER, vp_accept(&p, false));
    p = good();
    strcpy(p.locale, "en-US-x");
    TEST_ASSERT_EQUAL_INT(VP_ACCEPT_LOCALE_OTHER, vp_accept(&p, false));
    p = good();
    strcpy(p.voice_id, "qwen3-designed_male");
    TEST_ASSERT_EQUAL_INT(VP_ACCEPT_VOICE_OTHER, vp_accept(&p, true));
    p = good();
    strcpy(p.voice_id, "qwen3-designed_female2");
    TEST_ASSERT_EQUAL_INT(VP_ACCEPT_VOICE_OTHER, vp_accept(&p, false));
    p = good();
    strcpy(p.voice_id, "qwen3-designed");
    TEST_ASSERT_EQUAL_INT(VP_ACCEPT_VOICE_OTHER, vp_accept(&p, false));
}

/* AUD-027: the key is frozen (records written by one firmware are found by the next), short
 * enough for NVS, and changes with the revision. */
static void test_identity_key(void)
{
    vp_pack_t p = good();
    uint8_t d[VP_DIGEST_SIZE];
    digest(d, 7u);
    vp_identity_t id;
    vp_identity_of(&p, d, &id);
    char key[VP_ID_KEY_SIZE];
    vp_identity_key(&id, key);
    TEST_ASSERT_EQUAL_STRING("v685bf419", key);
    TEST_ASSERT_TRUE(strlen(key) <= 15u);
    id.revision = 2u;
    vp_identity_key(&id, key);
    TEST_ASSERT_EQUAL_STRING("vb888332a", key);
    /* the digest is not part of the key: other audio under the same identity finds its record */
    digest(id.sha256, 99u);
    vp_identity_key(&id, key);
    TEST_ASSERT_EQUAL_STRING("vb888332a", key);
}

/* AUD-027: the record's bytes are a stored format - this vector is frozen. */
static void test_identity_record_frozen(void)
{
    static const uint8_t v1[VP_ID_RECORD_SIZE] = {
        0x01, 0x00, 0x00, 0x00, 0x2A, 0x01, 0x00, 0x00,                                        /* format, rev 298 */
        'e', 'n', '-', 'U', 'S', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,                              /* locale[16] */
        'q', 'w', 'e', 'n', '3', '-', 'd', 'e', 's', 'i', 'g', 'n', 'e', 'd', '_', 'f',
        'e', 'm', 'a', 'l', 'e', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,                              /* voice_id[32] */
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
        16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,                        /* sha256 */
    };
    vp_pack_t p = good();
    p.pack_revision = 298u;
    uint8_t d[VP_DIGEST_SIZE];
    digest(d, 0u);
    vp_identity_t id;
    vp_identity_of(&p, d, &id);
    uint8_t rec[VP_ID_RECORD_SIZE];
    vp_identity_encode(&id, rec);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(v1, rec, VP_ID_RECORD_SIZE);

    vp_identity_t back;
    TEST_ASSERT_TRUE(vp_identity_decode(v1, sizeof(v1), &back));
    TEST_ASSERT_EQUAL_MEMORY(&id, &back, sizeof(id));
}

/* AUD-027: a truncated, oversized, newer-format or damaged record is not read as an identity. */
static void test_identity_record_refused(void)
{
    vp_pack_t p = good();
    uint8_t d[VP_DIGEST_SIZE];
    digest(d, 1u);
    vp_identity_t id, back;
    vp_identity_of(&p, d, &id);
    uint8_t rec[VP_ID_RECORD_SIZE + 1u];
    vp_identity_encode(&id, rec);
    rec[VP_ID_RECORD_SIZE] = 0u;
    TEST_ASSERT_FALSE(vp_identity_decode(rec, VP_ID_RECORD_SIZE - 1u, &back));
    TEST_ASSERT_FALSE(vp_identity_decode(rec, VP_ID_RECORD_SIZE + 1u, &back));
    TEST_ASSERT_FALSE(vp_identity_decode(NULL, VP_ID_RECORD_SIZE, &back));

    uint8_t bad[VP_ID_RECORD_SIZE];
    memcpy(bad, rec, sizeof(bad));
    bad[0] = 2u; /* a later format */
    TEST_ASSERT_FALSE(vp_identity_decode(bad, sizeof(bad), &back));
    memcpy(bad, rec, sizeof(bad));
    bad[2] = 1u; /* a reserved byte */
    TEST_ASSERT_FALSE(vp_identity_decode(bad, sizeof(bad), &back));
    memcpy(bad, rec, sizeof(bad));
    memset(&bad[8], 'x', VP_LOCALE_SIZE); /* unterminated locale */
    TEST_ASSERT_FALSE(vp_identity_decode(bad, sizeof(bad), &back));
    memcpy(bad, rec, sizeof(bad));
    bad[24] = 0u; /* empty voice */
    TEST_ASSERT_FALSE(vp_identity_decode(bad, sizeof(bad), &back));
    memcpy(bad, rec, sizeof(bad));
    bad[8 + 6] = 'z'; /* bytes after the locale's NUL */
    TEST_ASSERT_FALSE(vp_identity_decode(bad, sizeof(bad), &back));
    TEST_ASSERT_TRUE(vp_identity_decode(rec, VP_ID_RECORD_SIZE, &back));
}

/* AUD-027: a known identity with another digest is refused; the same one is a reinstall; a key
 * holding another identity (or an unreadable record) is a collision. */
static void test_identity_judge(void)
{
    vp_pack_t p = good();
    uint8_t d[VP_DIGEST_SIZE];
    digest(d, 3u);
    vp_identity_t cand, stored;
    vp_identity_of(&p, d, &cand);

    TEST_ASSERT_EQUAL_INT(VP_ID_NEW, vp_identity_judge(NULL, false, &cand));
    stored = cand;
    TEST_ASSERT_EQUAL_INT(VP_ID_SAME, vp_identity_judge(&stored, true, &cand));
    stored.sha256[31] ^= 1u;
    TEST_ASSERT_EQUAL_INT(VP_ID_OTHER_AUDIO, vp_identity_judge(&stored, true, &cand));
    TEST_ASSERT_EQUAL_STRING("other_audio", vp_id_verdict_name(VP_ID_OTHER_AUDIO));

    stored = cand;
    stored.revision = 2u;
    TEST_ASSERT_EQUAL_INT(VP_ID_COLLISION, vp_identity_judge(&stored, true, &cand));
    stored = cand;
    stored.locale[1] = 'N';
    TEST_ASSERT_EQUAL_INT(VP_ID_COLLISION, vp_identity_judge(&stored, true, &cand));
    stored = cand;
    stored.voice_id[0] = 'Q';
    TEST_ASSERT_EQUAL_INT(VP_ID_COLLISION, vp_identity_judge(&stored, true, &cand));
    stored = cand;
    TEST_ASSERT_EQUAL_INT(VP_ID_COLLISION, vp_identity_judge(&stored, false, &cand));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_accept_fixture);
    RUN_TEST(test_accept_draft);
    RUN_TEST(test_accept_other_voice);
    RUN_TEST(test_identity_key);
    RUN_TEST(test_identity_record_frozen);
    RUN_TEST(test_identity_record_refused);
    RUN_TEST(test_identity_judge);
    return UNITY_END();
}
