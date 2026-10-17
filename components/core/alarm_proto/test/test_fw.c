/*
 * test_fw.c - firmware update helpers (alarm_proto fw.c): versions, the release manifest, the
 * install policy, the serial upload framing and the Home Assistant update entity, plus the
 * panel's retry schedule for failed release checks and its self-test's rule.
 * Requirements: OTA-002, OTA-004, OTA-005, OTA-010, OTA-013, OTA-014, OTA-015 (docs/features/13),
 * LINK-041 (docs/features/11)
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "alarm_proto/proto_fw.h"
#include "unity.h"

/* The retry schedule and the signed-profile rule the update task uses, portable parts of
 * alarm_proto since docs/features/33 step 8, and the self-test's rule (ota_selftest.h). */
#include "alarm_proto/ota_retry.h"
#include "alarm_proto/ota_profile.h"
#include "alarm_proto/ota_selftest.h"

/* DEV-004 / DEV-015: signed identity, not version forcing, determines the allowed profile. */
static void test_signed_profiles(void)
{
    TEST_ASSERT_TRUE(ota_profile_accepts("alarm_crowpanel_p4-release", "alarm_crowpanel_p4-release"));
    TEST_ASSERT_FALSE(ota_profile_accepts("alarm_crowpanel_p4-release", "alarm_crowpanel_p4-dev"));
    TEST_ASSERT_FALSE(ota_profile_accepts("alarm_crowpanel_p4-release", "alarm_crowpanel_p4"));
    TEST_ASSERT_FALSE(ota_profile_accepts("alarm_crowpanel_p4-release", "alarm_crowpanel_p4-releaseX"));
    TEST_ASSERT_TRUE(ota_profile_accepts("alarm_crowpanel_p4-dev", "alarm_crowpanel_p4-dev"));
    TEST_ASSERT_TRUE(ota_profile_accepts("alarm_crowpanel_p4-dev", "alarm_crowpanel_p4-release"));
    TEST_ASSERT_FALSE(ota_profile_accepts("alarm_crowpanel_p4-dev", "alarm_other-dev"));
    TEST_ASSERT_FALSE(ota_profile_accepts("alarm_crowpanel_p4-dev", "alarm_crowpanel_p4"));
    TEST_ASSERT_TRUE(ota_profile_accepts("alarm_other-dev", "alarm_other-release"));
    TEST_ASSERT_FALSE(ota_profile_accepts("alarm_other-dev", "alarm_crowpanel_p4-release"));
    TEST_ASSERT_FALSE(ota_profile_accepts("alarm_other-release", "alarm_other-dev"));
    TEST_ASSERT_FALSE(ota_profile_accepts("", "-release"));
}

static const proto_ident_t ID = {"alarmpanel", "a1b2c3", "0.6.0", "CrowPanel Advanced 7"};

static char BUF[PROTO_PAYLOAD_MAX];

void setUp(void) {}
void tearDown(void) {}

/* ---- versions (OTA-014) ------------------------------------------------------------------ */

static int cmp(const char *a, const char *b)
{
    proto_version_t va;
    proto_version_t vb;
    TEST_ASSERT_TRUE_MESSAGE(proto_version_parse(a, &va), a);
    TEST_ASSERT_TRUE_MESSAGE(proto_version_parse(b, &vb), b);
    return proto_version_compare(&va, &vb);
}

/* OTA-014 */
static void test_version_parse(void)
{
    proto_version_t v;
    TEST_ASSERT_TRUE(proto_version_parse("1.2.3", &v));
    TEST_ASSERT_EQUAL_UINT16(1, v.major);
    TEST_ASSERT_EQUAL_UINT16(2, v.minor);
    TEST_ASSERT_EQUAL_UINT16(3, v.patch);
    TEST_ASSERT_EQUAL_STRING("", v.pre);
    TEST_ASSERT_TRUE(proto_version_parse("v0.6.1-test", &v));
    TEST_ASSERT_EQUAL_UINT16(6, v.minor);
    TEST_ASSERT_EQUAL_STRING("test", v.pre);
    TEST_ASSERT_TRUE(proto_version_parse("1.0.0-rc.1+build.7", &v));
    TEST_ASSERT_EQUAL_STRING("rc.1", v.pre);

    const char *bad[] = {"", "1", "1.2", "1.2.3.4", "01.2.3", "1.2.x", "1.2.3-", "1.2.3+",
                         "834e0ad", "1.2.3 ", "70000.0.0", "1.2.3-rc_1"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(proto_version_parse(bad[i], &v), bad[i]);
    }
    TEST_ASSERT_FALSE(proto_version_parse(NULL, &v));
}

/* OTA-014 */
static void test_version_order(void)
{
    TEST_ASSERT_TRUE(cmp("0.6.0", "0.6.1") < 0);
    TEST_ASSERT_TRUE(cmp("0.10.0", "0.9.9") > 0);
    TEST_ASSERT_TRUE(cmp("1.0.0", "0.99.99") > 0);
    TEST_ASSERT_EQUAL_INT(0, cmp("1.2.3", "v1.2.3+abc"));
    /* semver 11.4: 1.0.0-alpha < -alpha.1 < -alpha.beta < -beta < -beta.2 < -beta.11 < -rc.1 */
    const char *chain[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
                           "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0"};
    for (size_t i = 0; i + 1u < sizeof(chain) / sizeof(chain[0]); i++) {
        TEST_ASSERT_TRUE_MESSAGE(cmp(chain[i], chain[i + 1u]) < 0, chain[i]);
        TEST_ASSERT_TRUE_MESSAGE(cmp(chain[i + 1u], chain[i]) > 0, chain[i]);
    }
    TEST_ASSERT_TRUE(cmp("0.6.1-test", "0.6.0") > 0);
}

/* OTA-014 */
static void test_version_upgrade_policy(void)
{
    TEST_ASSERT_TRUE(proto_version_is_upgrade("0.6.0", "0.6.1", false));
    TEST_ASSERT_FALSE(proto_version_is_upgrade("0.6.1", "0.6.1", false));
    TEST_ASSERT_FALSE(proto_version_is_upgrade("0.6.1", "0.6.0", false));
    TEST_ASSERT_TRUE(proto_version_is_upgrade("0.6.1", "0.6.0", true));
    TEST_ASSERT_FALSE(proto_version_is_upgrade("0.6.0", "garbage", false));
    TEST_ASSERT_TRUE(proto_version_is_upgrade("834e0ad-dirty", "0.6.0", true));
    TEST_ASSERT_FALSE(proto_version_is_upgrade("834e0ad-dirty", "0.6.0", false));
}

/* ---- manifest (OTA-002) ------------------------------------------------------------------ */

static const char MANIFEST[] =
    "{\"version\":\"0.6.1\",\"notes_url\":\"https://example.org/notes/0.6.1\",\"artifacts\":["
    "{\"device\":\"cyd_esp32\",\"url\":\"cyd.bin\",\"size\":100},"
    "{\"device\":\"crowpanel_p4\",\"chip_rev\":\"v3\",\"url\":\"p4_rev3.bin\",\"size\":2},"
    "{\"device\":\"crowpanel_p4\",\"chip_rev\":\"v1\",\"url\":\"p4_rev1.bin\",\"size\":1958464,"
    "\"sha256\":\"00112233445566778899aabbccddeeff00112233445566778899AABBCCDDEEFF\","
    "\"future\":{\"x\":[1,2]}}]}";

/* OTA-002 */
static void test_manifest_pick(void)
{
    proto_fw_artifact_t a;
    TEST_ASSERT_EQUAL(ALARM_OK,
                      proto_fw_manifest_pick(MANIFEST, strlen(MANIFEST), "crowpanel_p4", "v1",
                                             "https://fw.example.org/rel/manifest.json", &a));
    TEST_ASSERT_EQUAL_STRING("0.6.1", a.version);
    TEST_ASSERT_EQUAL_STRING("https://fw.example.org/rel/p4_rev1.bin", a.url);
    TEST_ASSERT_EQUAL_UINT32(1958464u, a.size);
    TEST_ASSERT_TRUE(a.has_sha256);
    TEST_ASSERT_EQUAL_HEX8(0x00, a.sha256[0]);
    TEST_ASSERT_EQUAL_HEX8(0x11, a.sha256[1]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, a.sha256[31]);
    TEST_ASSERT_EQUAL_STRING("https://example.org/notes/0.6.1", a.notes_url);

    TEST_ASSERT_EQUAL(ALARM_OK,
                      proto_fw_manifest_pick(MANIFEST, strlen(MANIFEST), "crowpanel_p4", "v3",
                                             "http://192.168.1.5:8000/manifest.json", &a));
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.5:8000/p4_rev3.bin", a.url);
    TEST_ASSERT_FALSE(a.has_sha256);

    /* devices with one image ignore chip_rev */
    TEST_ASSERT_EQUAL(ALARM_OK, proto_fw_manifest_pick(MANIFEST, strlen(MANIFEST), "cyd_esp32",
                                                       NULL, "http://h/x/m.json", &a));
    TEST_ASSERT_EQUAL_STRING("http://h/x/cyd.bin", a.url);

    TEST_ASSERT_EQUAL(ALARM_ERR_DISABLED,
                      proto_fw_manifest_pick(MANIFEST, strlen(MANIFEST), "h2_radio", NULL,
                                             "http://h/m.json", &a));
}

/* OTA-002 */
static void test_manifest_urls_and_errors(void)
{
    proto_fw_artifact_t a;
    const char *abs = "{\"artifacts\":[{\"device\":\"d\",\"version\":\"1.0.0\","
                      "\"url\":\"https://cdn.example.org/a.bin\"}]}";
    TEST_ASSERT_EQUAL(ALARM_OK, proto_fw_manifest_pick(abs, strlen(abs), "d", NULL,
                                                       "http://h/m.json", &a));
    TEST_ASSERT_EQUAL_STRING("https://cdn.example.org/a.bin", a.url);
    TEST_ASSERT_EQUAL_STRING("1.0.0", a.version);

    const char *root = "{\"version\":\"1.0.0\",\"artifacts\":[{\"device\":\"d\","
                       "\"url\":\"/files/a.bin\"}]}";
    TEST_ASSERT_EQUAL(ALARM_OK, proto_fw_manifest_pick(root, strlen(root), "d", NULL,
                                                       "https://h:8443/rel/m.json", &a));
    TEST_ASSERT_EQUAL_STRING("https://h:8443/files/a.bin", a.url);

    const char *bad[] = {
        "",
        "[]",
        "{\"version\":\"1.0.0\",\"artifacts\":[{\"device\":\"d\",\"url\":\"a.bin\"}],}",
        "{\"version\":\"1.0\",\"artifacts\":[{\"device\":\"d\",\"url\":\"a.bin\"}]}",
        "{\"version\":\"1.0.0\",\"artifacts\":[{\"device\":\"d\"}]}",
        "{\"version\":\"1.0.0\",\"artifacts\":[{\"device\":\"d\",\"url\":\"a\",\"size\":-1}]}",
        "{\"version\":\"1.0.0\",\"artifacts\":[{\"device\":\"d\",\"url\":\"a\",\"sha256\":\"ab\"}]}",
        "{\"version\":\"1.0.0\",\"artifacts\":{\"device\":\"d\"}}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(ALARM_ERR_BAD_ARG,
                                  proto_fw_manifest_pick(bad[i], strlen(bad[i]), "d", NULL,
                                                         "http://h/m.json", &a),
                                  bad[i]);
    }
}

/* OTA-016, I18N-047 */
static void test_manifest_languages(void)
{
    proto_fw_language_t l[4];
    const char *murl = "https://fw.example.org/rel/manifest.json";
    const char *sha = "00112233445566778899aabbccddeeff00112233445566778899AABBCCDDEEFF";
    char js[1024];

    /* the artifact pick is unaffected by a languages key; no key is 0 entries */
    TEST_ASSERT_EQUAL_UINT(0, proto_fw_manifest_languages(MANIFEST, strlen(MANIFEST), murl, l, 4));

    snprintf(js, sizeof(js),
             "{\"version\":\"0.6.1\",\"languages\":["
             "{\"locale\":\"es-US\",\"url\":\"p-es.robolang\",\"size\":1000,\"sha256\":\"%s\"},"
             "{\"locale\":\"en-GB\",\"url\":\"https://cdn.example.org/en.robolang\","
             "\"version\":\"0.6.0\",\"future\":[1]}],"
             "\"artifacts\":[{\"device\":\"d\",\"url\":\"a.bin\"}]}",
             sha);
    proto_fw_artifact_t a;
    TEST_ASSERT_EQUAL(ALARM_OK, proto_fw_manifest_pick(js, strlen(js), "d", NULL, murl, &a));
    TEST_ASSERT_EQUAL_STRING("https://fw.example.org/rel/a.bin", a.url);
    TEST_ASSERT_EQUAL_UINT(2, proto_fw_manifest_languages(js, strlen(js), murl, l, 4));
    TEST_ASSERT_EQUAL_STRING("es-US", l[0].locale);
    TEST_ASSERT_EQUAL_STRING("https://fw.example.org/rel/p-es.robolang", l[0].url);
    TEST_ASSERT_EQUAL_STRING("0.6.1", l[0].version);
    TEST_ASSERT_EQUAL_UINT32(1000u, l[0].size);
    TEST_ASSERT_TRUE(l[0].has_sha256);
    TEST_ASSERT_EQUAL_HEX8(0x11, l[0].sha256[1]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, l[0].sha256[31]);
    TEST_ASSERT_EQUAL_STRING("en-GB", l[1].locale);
    TEST_ASSERT_EQUAL_STRING("https://cdn.example.org/en.robolang", l[1].url);
    TEST_ASSERT_EQUAL_STRING("0.6.0", l[1].version);
    TEST_ASSERT_EQUAL_UINT32(0u, l[1].size);
    TEST_ASSERT_FALSE(l[1].has_sha256);

    /* more entries than max */
    TEST_ASSERT_EQUAL_UINT(1, proto_fw_manifest_languages(js, strlen(js), murl, l, 1));
    TEST_ASSERT_EQUAL_STRING("es-US", l[0].locale);

    /* skipped entries, each followed by a good one */
    const char *skip[] = {
        "{\"locale\":\"\",\"url\":\"u\"}",
        "{\"url\":\"u\"}",
        "{\"locale\":\"x\",\"url\":\"u\"}",
        "{\"locale\":\"abcdefghijklmnop\",\"url\":\"u\"}",
        "{\"locale\":\"es_US\",\"url\":\"u\"}",
        "{\"locale\":\"es-US\"}",
        "{\"locale\":\"es-US\",\"url\":\"\"}",
        "{\"locale\":\"es-US\",\"url\":\"u\",\"sha256\":\"00112233445566778899aabbccddeeff"
        "00112233445566778899aabbccddeef\"}",
        "{\"locale\":\"es-US\",\"url\":\"u\",\"sha256\":\"zz112233445566778899aabbccddeeff"
        "00112233445566778899aabbccddeeff\"}",
        "{\"locale\":\"es-US\",\"url\":\"u\",\"size\":-1}",
        "{\"locale\":\"es-US\",\"url\":\"u\",\"size\":1.5}",
        "{\"locale\":\"es-US\",\"url\":\"u\",\"size\":\"9\"}",
    };
    for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) {
        snprintf(js, sizeof(js), "{\"languages\":[%s,{\"locale\":\"de\",\"url\":\"d\"}]}", skip[i]);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1, proto_fw_manifest_languages(js, strlen(js), murl, l, 4),
                                       skip[i]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("de", l[0].locale, skip[i]);
    }
    /* a relative URL that does not fit once resolved against the manifest's */
    char longurl[251];
    memset(longurl, 'a', sizeof(longurl) - 1u);
    longurl[sizeof(longurl) - 1u] = '\0';
    snprintf(js, sizeof(js), "{\"languages\":[{\"locale\":\"es-US\",\"url\":\"%s\"},"
                             "{\"locale\":\"de\",\"url\":\"d\"}]}", longurl);
    TEST_ASSERT_EQUAL_UINT(1, proto_fw_manifest_languages(js, strlen(js), murl, l, 4));
    TEST_ASSERT_EQUAL_STRING("de", l[0].locale);

    /* not valid */
    const char *bad[] = {
        "", "[]", "{\"languages\":{\"locale\":\"de\"}}", "{\"languages\":[1]}",
        "{\"languages\":[{\"locale\":\"de\",\"url\":\"d\"}],}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_UINT_MESSAGE(SIZE_MAX,
                                       proto_fw_manifest_languages(bad[i], strlen(bad[i]), murl, l, 4),
                                       bad[i]);
    }
}

/* OPT-007, OTA-016: the manifest's asset packs follow the languages' rules */
static void test_manifest_assets(void)
{
    proto_fw_asset_t e[4];
    const char *murl = "https://fw.example.org/rel/manifest.json";
    const char *sha = "00112233445566778899aabbccddeeff00112233445566778899AABBCCDDEEFF";
    char js[1400];

    /* no key is 0 entries; the languages are not assets */
    TEST_ASSERT_EQUAL_UINT(0, proto_fw_manifest_assets(MANIFEST, strlen(MANIFEST), murl, e, 4));

    snprintf(js, sizeof(js),
             "{\"version\":\"0.9.0\",\"languages\":[{\"locale\":\"de\",\"url\":\"d\"}],"
             "\"assets\":["
             "{\"kind\":\"letters\",\"url\":\"r-0.9.0-letters.roboasset\",\"size\":98304,"
             "\"sha256\":\"%s\"},"
             "{\"kind\":\"web\",\"url\":\"w.roboasset\"},"
             "{\"kind\":\"picture\",\"url\":\"https://cdn.example.org/saver.roboasset\","
             "\"version\":\"0.8.0\",\"future\":{\"x\":1}},"
             "{\"kind\":\"certificates\",\"url\":\"/c/certs.roboasset\"}],"
             "\"artifacts\":[{\"device\":\"d\",\"url\":\"a.bin\"}]}",
             sha);
    proto_fw_artifact_t a;
    TEST_ASSERT_EQUAL(ALARM_OK, proto_fw_manifest_pick(js, strlen(js), "d", NULL, murl, &a));
    proto_fw_language_t l[2];
    TEST_ASSERT_EQUAL_UINT(1, proto_fw_manifest_languages(js, strlen(js), murl, l, 2));
    TEST_ASSERT_EQUAL_STRING("de", l[0].locale);
    TEST_ASSERT_EQUAL_UINT(3, proto_fw_manifest_assets(js, strlen(js), murl, e, 4));
    TEST_ASSERT_EQUAL(PROTO_FW_ASSET_LETTERS, e[0].kind);
    TEST_ASSERT_EQUAL_STRING("https://fw.example.org/rel/r-0.9.0-letters.roboasset", e[0].url);
    TEST_ASSERT_EQUAL_STRING("0.9.0", e[0].version);
    TEST_ASSERT_EQUAL_UINT32(98304u, e[0].size);
    TEST_ASSERT_TRUE(e[0].has_sha256);
    TEST_ASSERT_EQUAL_HEX8(0xFF, e[0].sha256[31]);
    TEST_ASSERT_EQUAL(PROTO_FW_ASSET_PICTURE, e[1].kind); /* "web" skipped, for now */
    TEST_ASSERT_EQUAL_STRING("https://cdn.example.org/saver.roboasset", e[1].url);
    TEST_ASSERT_EQUAL_STRING("0.8.0", e[1].version);
    TEST_ASSERT_FALSE(e[1].has_sha256);
    TEST_ASSERT_EQUAL(PROTO_FW_ASSET_CERTS, e[2].kind);
    TEST_ASSERT_EQUAL_STRING("https://fw.example.org/c/certs.roboasset", e[2].url);
    TEST_ASSERT_EQUAL_UINT32(0u, e[2].size);

    /* more entries than max */
    TEST_ASSERT_EQUAL_UINT(1, proto_fw_manifest_assets(js, strlen(js), murl, e, 1));
    TEST_ASSERT_EQUAL(PROTO_FW_ASSET_LETTERS, e[0].kind);

    /* skipped entries, each followed by a good one */
    const char *skip[] = {
        "{\"url\":\"u\"}",
        "{\"kind\":\"\",\"url\":\"u\"}",
        "{\"kind\":\"web\",\"url\":\"u\"}",
        "{\"kind\":\"Letters\",\"url\":\"u\"}",
        "{\"kind\":\"letters-and-more-than-fits\",\"url\":\"u\"}",
        "{\"kind\":3,\"url\":\"u\"}",
        "{\"kind\":\"letters\"}",
        "{\"kind\":\"letters\",\"url\":\"\"}",
        "{\"kind\":\"letters\",\"url\":\"u\",\"sha256\":\"00112233445566778899aabbccddeeff"
        "00112233445566778899aabbccddeef\"}",
        "{\"kind\":\"letters\",\"url\":\"u\",\"sha256\":\"zz112233445566778899aabbccddeeff"
        "00112233445566778899aabbccddeeff\"}",
        "{\"kind\":\"letters\",\"url\":\"u\",\"size\":-1}",
        "{\"kind\":\"letters\",\"url\":\"u\",\"size\":1.5}",
        "{\"kind\":\"letters\",\"url\":\"u\",\"size\":4294967296}",
        "{\"kind\":\"letters\",\"url\":\"u\",\"size\":\"9\"}",
    };
    for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) {
        snprintf(js, sizeof(js), "{\"assets\":[%s,{\"kind\":\"picture\",\"url\":\"p\"}]}", skip[i]);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1, proto_fw_manifest_assets(js, strlen(js), murl, e, 4),
                                       skip[i]);
        TEST_ASSERT_EQUAL_MESSAGE(PROTO_FW_ASSET_PICTURE, e[0].kind, skip[i]);
    }

    /* a relative URL that does not fit once resolved against the manifest's */
    char longurl[251];
    memset(longurl, 'a', sizeof(longurl) - 1u);
    longurl[sizeof(longurl) - 1u] = '\0';
    snprintf(js, sizeof(js), "{\"assets\":[{\"kind\":\"letters\",\"url\":\"%s\"},"
                             "{\"kind\":\"picture\",\"url\":\"p\"}]}", longurl);
    TEST_ASSERT_EQUAL_UINT(1, proto_fw_manifest_assets(js, strlen(js), murl, e, 4));
    TEST_ASSERT_EQUAL(PROTO_FW_ASSET_PICTURE, e[0].kind);

    /* a value that is not a list (null, as a server may send an empty one) reads as none, and the
     * languages beside it still read */
    const char *not_list[] = {"null", "7", "\"letters\"", "true", "{\"kind\":\"letters\",\"url\":\"l\"}"};
    for (size_t i = 0; i < sizeof(not_list) / sizeof(not_list[0]); i++) {
        snprintf(js, sizeof(js), "{\"languages\":[{\"locale\":\"de\",\"url\":\"d\"}],\"assets\":%s}",
                 not_list[i]);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0, proto_fw_manifest_assets(js, strlen(js), murl, e, 4), not_list[i]);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1, proto_fw_manifest_languages(js, strlen(js), murl, l, 2),
                                       not_list[i]);
    }

    /* not valid: a broken manifest is broken for both lists */
    const char *bad[] = {
        "", "[]", "{\"assets\":[1}", "{\"assets\":[{\"kind\":\"letters\",\"url\":\"d\"}],}",
        "{\"version\":1,\"assets\":[]}", "{\"assets\":nul}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_UINT_MESSAGE(SIZE_MAX,
                                       proto_fw_manifest_assets(bad[i], strlen(bad[i]), murl, e, 4),
                                       bad[i]);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(SIZE_MAX,
                                       proto_fw_manifest_languages(bad[i], strlen(bad[i]), murl, l, 2),
                                       bad[i]);
    }
    /* a truncated manifest at every length */
    snprintf(js, sizeof(js), "{\"version\":\"0.9.0\",\"assets\":[{\"kind\":\"letters\",\"url\":\"l\"}]}");
    for (size_t cut = 0; cut < strlen(js); cut++) {
        TEST_ASSERT_EQUAL_UINT(SIZE_MAX, proto_fw_manifest_assets(js, cut, murl, e, 4));
    }
}

/* ---- install policy (OTA-004) ------------------------------------------------------------ */

static alarm_snapshot_t SNAP;

static void idle(void)
{
    memset(&SNAP, 0, sizeof(SNAP));
    SNAP.partitions = 0x3u;
}

/* OTA-004 */
static void test_install_policy(void)
{
    const char *why = NULL;
    idle();
    TEST_ASSERT_TRUE(proto_fw_install_allowed(&SNAP, &why));
    TEST_ASSERT_EQUAL_STRING("", why);

    /* trouble beeps and alarm memory don't block an update */
    SNAP.part[0].sounder = ALARM_SOUND_TROUBLE;
    SNAP.part[0].memory = 1u;
    TEST_ASSERT_TRUE(proto_fw_install_allowed(&SNAP, NULL));

    idle();
    SNAP.part[1].state = ALARM_STATE_EXIT_DELAY;
    TEST_ASSERT_FALSE(proto_fw_install_allowed(&SNAP, &why));
    TEST_ASSERT_EQUAL_STRING("The system is armed", why);

    idle();
    SNAP.part[0].state = ALARM_STATE_ARMED;
    TEST_ASSERT_FALSE(proto_fw_install_allowed(&SNAP, NULL));

    /* a disabled partition's (stale) status is ignored */
    idle();
    SNAP.partitions = 0x1u;
    SNAP.part[1].state = ALARM_STATE_ARMED;
    TEST_ASSERT_TRUE(proto_fw_install_allowed(&SNAP, NULL));

    idle();
    SNAP.part[0].bell = ALARM_PAT_TEMPORAL3;
    TEST_ASSERT_FALSE(proto_fw_install_allowed(&SNAP, &why));
    TEST_ASSERT_EQUAL_STRING("An alarm is sounding", why);

    idle();
    SNAP.part[0].sounder = ALARM_SOUND_AUX;
    TEST_ASSERT_FALSE(proto_fw_install_allowed(&SNAP, NULL));

    idle();
    SNAP.part[1].walk_test = true;
    TEST_ASSERT_FALSE(proto_fw_install_allowed(&SNAP, &why));
    TEST_ASSERT_EQUAL_STRING("A walk test is running", why);

    idle();
    SNAP.program = true;
    TEST_ASSERT_FALSE(proto_fw_install_allowed(&SNAP, &why));
    TEST_ASSERT_EQUAL_STRING("Installer mode is on", why);
}

/* ---- retry after a failed release check (OTA-015) ---------------------------------------- */

/* OTA-015: one bad night must not stop the panel looking for updates for good. */
static void test_check_retry_schedule(void)
{
    const uint32_t day = 24u * 3600u * 1000u;
    const uint32_t hour = 3600u * 1000u;

    /* a failure always buys another attempt, and the wait doubles from ten minutes */
    TEST_ASSERT_EQUAL_UINT32(PANEL_OTA_RETRY_MIN_MS, panel_ota_retry_delay_ms(1u, day));
    TEST_ASSERT_EQUAL_UINT32(2u * PANEL_OTA_RETRY_MIN_MS, panel_ota_retry_delay_ms(2u, day));
    TEST_ASSERT_EQUAL_UINT32(4u * PANEL_OTA_RETRY_MIN_MS, panel_ota_retry_delay_ms(3u, day));
    TEST_ASSERT_EQUAL_UINT32(8u * PANEL_OTA_RETRY_MIN_MS, panel_ota_retry_delay_ms(4u, day));

    /* bounded: however long the outage lasts, the panel keeps looking every six hours */
    TEST_ASSERT_EQUAL_UINT32(PANEL_OTA_RETRY_MAX_MS, panel_ota_retry_delay_ms(20u, day));
    TEST_ASSERT_EQUAL_UINT32(PANEL_OTA_RETRY_MAX_MS, panel_ota_retry_delay_ms(4000000000u, day));

    /* a failure never makes it look less often than the scheduled interval */
    TEST_ASSERT_EQUAL_UINT32(hour, panel_ota_retry_delay_ms(9u, hour));
    TEST_ASSERT_EQUAL_UINT32(PANEL_OTA_RETRY_MIN_MS, panel_ota_retry_delay_ms(1u, hour));

    /* automatic checks switched off: a failed manual check still uses the ladder */
    TEST_ASSERT_EQUAL_UINT32(PANEL_OTA_RETRY_MIN_MS, panel_ota_retry_delay_ms(0u, 0u));
    TEST_ASSERT_EQUAL_UINT32(PANEL_OTA_RETRY_MIN_MS, panel_ota_retry_delay_ms(1u, 0u));
    TEST_ASSERT_EQUAL_UINT32(PANEL_OTA_RETRY_MAX_MS, panel_ota_retry_delay_ms(99u, 0u));

    /* the delay never runs away and never collapses to nothing */
    for (uint32_t fails = 0u; fails <= 40u; fails++) {
        const uint32_t d = panel_ota_retry_delay_ms(fails, day);
        TEST_ASSERT_TRUE(d >= PANEL_OTA_RETRY_MIN_MS && d <= PANEL_OTA_RETRY_MAX_MS);
    }
}

/* ---- the self-test after an update (OTA-005, docs/features/11 LINK-041) ----------------- */

static panel_ota_part_t part(bool reported, bool fresh, uint32_t covered_ms, uint32_t span_ms,
                             bool may_wait)
{
    const panel_ota_part_t p = {reported, fresh, covered_ms, span_ms, may_wait};
    return p;
}

/* OTA-005: a part that never reports or falls silent fails at once; one reporting, but not for
 * long enough, is waited for. LINK-041: a master's keypads may wait instead of failing. */
static void test_selftest_part_verdicts(void)
{
    const uint32_t span = PANEL_OTA_HEALTH_SPAN_MS;
    panel_ota_part_t p = part(false, false, 0u, span, false);
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_PART_MISSING, panel_ota_part_judge(&p));
    p = part(true, false, 60000u, span, false); /* went quiet */
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_PART_MISSING, panel_ota_part_judge(&p));
    p = part(true, true, span - 1u, span, false);
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_PART_UNPROVEN, panel_ota_part_judge(&p));
    p = part(true, true, span, span, false);
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_PART_OK, panel_ota_part_judge(&p));

    /* a part that may wait (a master's keypads before the deadline) is never missing */
    const uint32_t kspan = PANEL_OTA_KEYPADS_SPAN_MS;
    p = part(false, false, 0u, kspan, true); /* no keypad has answered yet */
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_PART_UNPROVEN, panel_ota_part_judge(&p));
    p = part(true, false, 40000u, kspan, true); /* answered, then a pause */
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_PART_UNPROVEN, panel_ota_part_judge(&p));
    p = part(true, true, kspan - 1u, kspan, true);
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_PART_UNPROVEN, panel_ota_part_judge(&p));
    p = part(true, true, kspan, kspan, true);
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_PART_OK, panel_ota_part_judge(&p));
    /* past the deadline it may not: still unanswered is missing */
    p = part(false, false, 0u, kspan, false);
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_PART_MISSING, panel_ota_part_judge(&p));

    /* the times it is built on: a keypad must have time to come back and prove itself */
    TEST_ASSERT_TRUE(PANEL_OTA_CONFIRM_MS < PANEL_OTA_CONFIRM_MAX_MS);
    TEST_ASSERT_TRUE(PANEL_OTA_KEYPADS_SPAN_MS <= PANEL_OTA_HEALTH_SPAN_MS);
    TEST_ASSERT_TRUE(PANEL_OTA_CONFIRM_MAX_MS - PANEL_OTA_KEYPADS_SPAN_MS >= 150000u);
}

/* The step after the verdicts: nothing before PANEL_OTA_CONFIRM_MS; confirmed once nothing is
 * missing or unproven; missing rolls back at once; unproven waits until the deadline. */
static void test_selftest_steps(void)
{
    const uint64_t until = PANEL_OTA_CONFIRM_MS;
    const uint64_t dl = PANEL_OTA_CONFIRM_MAX_MS;
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_SELFTEST_WAIT,
                          panel_ota_selftest_step(1u, 0u, false, until - 1u, until, dl));
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_SELFTEST_CONFIRM,
                          panel_ota_selftest_step(0u, 0u, false, until, until, dl));
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_SELFTEST_ROLL_BACK,
                          panel_ota_selftest_step(1u, 0u, false, until, until, dl));
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_SELFTEST_WAIT,
                          panel_ota_selftest_step(0u, 1u, false, dl - 1u, until, dl));
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_SELFTEST_ROLL_BACK,
                          panel_ota_selftest_step(0u, 1u, false, dl, until, dl));
    /* a remote install's confirmation waits as an unproven part does */
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_SELFTEST_WAIT,
                          panel_ota_selftest_step(0u, 0u, true, until, until, dl));
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_SELFTEST_ROLL_BACK,
                          panel_ota_selftest_step(0u, 0u, true, dl, until, dl));
}

#define NEVER UINT32_MAX

/* A master's self-test second by second, as its update task runs it (panel_ota.c health_judge):
 * its own part reports from own_from (NEVER: not at all); one keypad first answers at back_ms
 * (NEVER: none) and then every second, except during [gap_from, gap_to). The keypads' part may
 * wait while on probation before the deadline. Returns the step it ends in; *at_ms when. */
static panel_ota_selftest_t selftest_run(uint32_t back_ms, uint32_t gap_from, uint32_t gap_to,
                                         uint32_t own_from, uint32_t *at_ms)
{
    const uint32_t own_bit = 1u << 0;
    const uint32_t keypads_bit = 1u << 6;
    uint32_t kp_first = 0u;
    uint32_t kp_last = 0u;
    for (uint32_t now = 1000u; now <= PANEL_OTA_CONFIRM_MAX_MS + 30000u; now += 1000u) {
        if (back_ms != NEVER && now >= back_ms && !(now >= gap_from && now < gap_to)) {
            if (kp_first == 0u) {
                kp_first = now;
            }
            kp_last = now;
        }
        const bool own_up = own_from != NEVER && now >= own_from;
        const panel_ota_part_t own = part(own_up, own_up, own_up ? now - own_from : 0u,
                                          PANEL_OTA_HEALTH_SPAN_MS, false);
        const bool kp_fresh = kp_first != 0u && now - kp_last <= PANEL_OTA_HEALTH_STALE_MS;
        const panel_ota_part_t kp =
            part(kp_first != 0u, kp_fresh, kp_first != 0u ? kp_last - kp_first : 0u,
                 PANEL_OTA_KEYPADS_SPAN_MS, now < PANEL_OTA_CONFIRM_MAX_MS);
        const panel_ota_part_verdict_t vo = panel_ota_part_judge(&own);
        const panel_ota_part_verdict_t vk = panel_ota_part_judge(&kp);
        uint32_t missing = 0u;
        uint32_t unproven = 0u;
        missing |= vo == PANEL_OTA_PART_MISSING ? own_bit : 0u;
        missing |= vk == PANEL_OTA_PART_MISSING ? keypads_bit : 0u;
        unproven |= vo == PANEL_OTA_PART_UNPROVEN ? own_bit : 0u;
        unproven |= vk == PANEL_OTA_PART_UNPROVEN ? keypads_bit : 0u;
        const panel_ota_selftest_t step = panel_ota_selftest_step(
            missing, unproven, false, now, PANEL_OTA_CONFIRM_MS, PANEL_OTA_CONFIRM_MAX_MS);
        if (step != PANEL_OTA_SELFTEST_WAIT) {
            *at_ms = now;
            return step;
        }
    }
    *at_ms = 0u;
    return PANEL_OTA_SELFTEST_WAIT;
}

/* LINK-041 (decision 3): a master's self-test needs one of its keypads back, and a keypad that
 * comes back within the self-test counts - only a master none came back to goes back. */
static void test_selftest_keypads_come_back(void)
{
    uint32_t at = 0u;
    /* a keypad on line all along (or no keypad to wait for): confirmed at the 60 s mark */
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_SELFTEST_CONFIRM, selftest_run(1000u, 0u, 0u, 1000u, &at));
    TEST_ASSERT_EQUAL_UINT32(PANEL_OTA_CONFIRM_MS, at);
    /* back at 90 s, after the 60 s mark (Wi-Fi, the keypad's backoff): not rolled back at 60 s,
     * confirmed once its answers cover a heartbeat */
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_SELFTEST_CONFIRM, selftest_run(90000u, 0u, 0u, 1000u, &at));
    TEST_ASSERT_EQUAL_UINT32(90000u + PANEL_OTA_KEYPADS_SPAN_MS, at);
    /* back at 155 s: the 30 s span of the panel's own parts could no longer be covered */
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_SELFTEST_CONFIRM, selftest_run(155000u, 0u, 0u, 1000u, &at));
    TEST_ASSERT_EQUAL_UINT32(155000u + PANEL_OTA_KEYPADS_SPAN_MS, at);
    /* the last moment that still counts */
    const uint32_t last = PANEL_OTA_CONFIRM_MAX_MS - PANEL_OTA_KEYPADS_SPAN_MS;
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_SELFTEST_CONFIRM, selftest_run(last, 0u, 0u, 1000u, &at));
    TEST_ASSERT_EQUAL_UINT32(PANEL_OTA_CONFIRM_MAX_MS, at);
    /* too late to prove itself: back at the deadline */
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_SELFTEST_ROLL_BACK,
                          selftest_run(last + 1000u, 0u, 0u, 1000u, &at));
    TEST_ASSERT_EQUAL_UINT32(PANEL_OTA_CONFIRM_MAX_MS, at);
    /* no keypad back at all: back to the old version, at the deadline, never at the 60 s mark */
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_SELFTEST_ROLL_BACK, selftest_run(NEVER, 0u, 0u, 1000u, &at));
    TEST_ASSERT_EQUAL_UINT32(PANEL_OTA_CONFIRM_MAX_MS, at);
    /* answered at 20 s, then a pause over the 60 s mark: waited for, confirmed when it answers
     * again */
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_SELFTEST_CONFIRM,
                          selftest_run(20000u, 30000u, 75000u, 1000u, &at));
    TEST_ASSERT_EQUAL_UINT32(75000u, at);
    /* answered, then gone for good: back at the deadline */
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_SELFTEST_ROLL_BACK,
                          selftest_run(20000u, 25000u, NEVER, 1000u, &at));
    TEST_ASSERT_EQUAL_UINT32(PANEL_OTA_CONFIRM_MAX_MS, at);
    /* the panel's own parts keep their rule: one that never reports fails at the 60 s mark */
    TEST_ASSERT_EQUAL_INT(PANEL_OTA_SELFTEST_ROLL_BACK, selftest_run(1000u, 0u, 0u, NEVER, &at));
    TEST_ASSERT_EQUAL_UINT32(PANEL_OTA_CONFIRM_MS, at);
}

/* ---- serial upload (OTA-013) ------------------------------------------------------------- */

/* OTA-013 */
static void test_base64(void)
{
    uint8_t out[16];
    TEST_ASSERT_EQUAL_size_t(0u, proto_base64_decode("", 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(1u, proto_base64_decode("Zg==", 4, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY("f", out, 1);
    TEST_ASSERT_EQUAL_size_t(2u, proto_base64_decode("Zm8=", 4, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY("fo", out, 2);
    TEST_ASSERT_EQUAL_size_t(6u, proto_base64_decode("Zm9vYmFy", 8, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY("foobar", out, 6);
    TEST_ASSERT_EQUAL_size_t(3u, proto_base64_decode("+/8A", 4, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8(0xFB, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, out[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[2]);

    const char *bad[] = {"Zg", "Zg=", "Z===", "Zg==Zg==", "Zm9v YmFy", "Zm-v", "Z=g="};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_size_t_MESSAGE(SIZE_MAX,
                                         proto_base64_decode(bad[i], strlen(bad[i]), out,
                                                             sizeof(out)),
                                         bad[i]);
    }
    TEST_ASSERT_EQUAL_size_t(SIZE_MAX, proto_base64_decode("Zm9vYmFy", 8, out, 5));
}

/* OTA-013 */
static void test_upload_line(void)
{
    uint8_t out[64];
    uint32_t seq = 0;
    /* zlib.crc32(b"hello world") == 0x0d4a1185 */
    TEST_ASSERT_EQUAL_size_t(11u, proto_fw_line_decode("17 0d4a1185 aGVsbG8gd29ybGQ=\r\n", &seq,
                                                       out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT32(17u, seq);
    TEST_ASSERT_EQUAL_MEMORY("hello world", out, 11);
    TEST_ASSERT_EQUAL_size_t(11u, proto_fw_line_decode("0 0D4A1185 aGVsbG8gd29ybGQ=", &seq, out,
                                                       sizeof(out)));
    TEST_ASSERT_EQUAL_UINT32(0u, seq);

    const char *bad[] = {
        "17 0d4a1186 aGVsbG8gd29ybGQ=", /* CRC mismatch */
        "17 0d4a118 aGVsbG8gd29ybGQ=",  /* short CRC */
        "x 0d4a1185 aGVsbG8gd29ybGQ=",
        "17  0d4a1185 aGVsbG8gd29ybGQ=",
        "17 0d4a1185 aGVsbG8gd29ybGQ",
        "4294967296 0d4a1185 aGVsbG8gd29ybGQ=",
        "",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_size_t_MESSAGE(SIZE_MAX,
                                         proto_fw_line_decode(bad[i], &seq, out, sizeof(out)),
                                         bad[i]);
    }
    TEST_ASSERT_EQUAL_size_t(SIZE_MAX, proto_fw_line_decode("1 0d4a1185 aGVsbG8gd29ybGQ=", &seq,
                                                            out, 10));
}

/* ---- Home Assistant update entity (OTA-010) ---------------------------------------------- */

static const char *build_state(const proto_fw_state_t *st)
{
    pj_writer_t w;
    pj_init(&w, BUF, sizeof(BUF));
    proto_fw_state(&w, st);
    TEST_ASSERT_NOT_EQUAL(0u, pj_finish(&w));
    return BUF;
}

/* OTA-010; without a release URL the key is left out, since Home Assistant refuses the whole
 * payload over "release_url":null (B019) */
static void test_update_state(void)
{
    proto_fw_state_t st = {"0.6.0", "", "RoboAlarms Panel firmware", NULL, false, -1};
    TEST_ASSERT_EQUAL_STRING("{\"installed_version\":\"0.6.0\",\"latest_version\":\"0.6.0\","
                             "\"title\":\"RoboAlarms Panel firmware\","
                             "\"in_progress\":false,\"update_percentage\":null}",
                             build_state(&st));
    st.release_url = "";
    TEST_ASSERT_NULL(strstr(build_state(&st), "release_url"));
    st.latest = "0.6.1";
    st.release_url = "https://example.org/notes/0.6.1";
    st.in_progress = true;
    st.percent = 42;
    TEST_ASSERT_EQUAL_STRING("{\"installed_version\":\"0.6.0\",\"latest_version\":\"0.6.1\","
                             "\"title\":\"RoboAlarms Panel firmware\","
                             "\"release_url\":\"https://example.org/notes/0.6.1\","
                             "\"in_progress\":true,\"update_percentage\":42}",
                             build_state(&st));
}

/* OTA-010 */
static void test_update_discovery(void)
{
    char topic[PROTO_TOPIC_LEN];
    TEST_ASSERT_TRUE(proto_ha_topic(&ID, "update", "firmware", topic, sizeof(topic)));
    TEST_ASSERT_EQUAL_STRING("homeassistant/update/alarmpanel_a1b2c3/firmware/config", topic);

    pj_writer_t w;
    pj_init(&w, BUF, sizeof(BUF));
    proto_ha_update(&w, &ID, true);
    TEST_ASSERT_NOT_EQUAL(0u, pj_finish(&w));
    TEST_ASSERT_NOT_NULL(strstr(BUF, "\"name\":\"Firmware\""));
    TEST_ASSERT_NOT_NULL(strstr(BUF, "\"unique_id\":\"alarmpanel_a1b2c3_firmware\""));
    TEST_ASSERT_NOT_NULL(strstr(BUF, "\"state_topic\":\"~/firmware\""));
    TEST_ASSERT_NOT_NULL(strstr(BUF, "\"device_class\":\"firmware\""));
    TEST_ASSERT_NOT_NULL(strstr(BUF, "\"command_topic\":\"~/firmware/set\""));
    TEST_ASSERT_NOT_NULL(strstr(BUF, "\"payload_install\":\"install\""));
    TEST_ASSERT_NOT_NULL(strstr(BUF, "\"sw_version\":\"0.6.0\""));

    pj_init(&w, BUF, sizeof(BUF));
    proto_ha_update(&w, &ID, false);
    TEST_ASSERT_NOT_EQUAL(0u, pj_finish(&w));
    TEST_ASSERT_NULL(strstr(BUF, "command_topic"));
}

/* ---- updates through the panel link (docs/features/11 "Updates through the link") -------- */

/* The why numbers are on the wire (FW_STATE) and the same as panel_link's PLINK_FW_WHY_*: frozen
 * (LINK-037, LINK-039). */
static void test_why_numbers_frozen(void)
{
    static const struct {
        proto_fw_why_t why;
        unsigned value;
    } FROZEN[] = {
        {PROTO_FW_WHY_OK, 0u},           {PROTO_FW_WHY_BROKE_OFF, 20u},
        {PROTO_FW_WHY_TOO_SLOW, 21u},    {PROTO_FW_WHY_BAD_SIZE, 22u},
        {PROTO_FW_WHY_BAD_SHA, 23u},     {PROTO_FW_WHY_STALLED, 24u},
        {PROTO_FW_WHY_TIMEOUT, 25u},     {PROTO_FW_WHY_NO_MEMORY, 26u},
        {PROTO_FW_WHY_FLASH, 27u},       {PROTO_FW_WHY_KEY, 30u},
        {PROTO_FW_WHY_CHIP, 31u},        {PROTO_FW_WHY_PROJECT, 32u},
        {PROTO_FW_WHY_NOT_NEWER, 33u},   {PROTO_FW_WHY_SIGNATURE, 34u},
        {PROTO_FW_WHY_TOO_BIG, 35u},     {PROTO_FW_WHY_UNKNOWN_KIND, 36u},
        {PROTO_FW_WHY_UNKNOWN_OP, 37u},  {PROTO_FW_WHY_BUDGET, 38u},
        {PROTO_FW_WHY_BAD_ORDER, 40u},   {PROTO_FW_WHY_WRONG_VERSION, 41u},
        {PROTO_FW_WHY_MASTER_BUSY, 50u}, {PROTO_FW_WHY_NOT_QUIET, 51u},
        {PROTO_FW_WHY_PANIC_PENDING, 52u}, {PROTO_FW_WHY_KEPT_TONE, 53u},
        {PROTO_FW_WHY_NO_STATUS, 54u},   {PROTO_FW_WHY_BUSY, 55u},
        {PROTO_FW_WHY_STOPPED, 60u},     {PROTO_FW_WHY_CANCELLED, 61u},
    };
    for (size_t i = 0; i < sizeof(FROZEN) / sizeof(FROZEN[0]); i++) {
        TEST_ASSERT_EQUAL_UINT(FROZEN[i].value, (unsigned)FROZEN[i].why);
    }
    TEST_ASSERT_EQUAL_UINT(0u, (unsigned)PROTO_FW_CAP_NONE);
    TEST_ASSERT_EQUAL_UINT(1u, (unsigned)PROTO_FW_CAP_UNKNOWN);
    TEST_ASSERT_EQUAL_UINT(2u, (unsigned)PROTO_FW_CAP_AT);
}

/* LINK-029: a paired keypad never updates past its master (decision 5). */
static void test_version_cap(void)
{
    /* no master: no cap, even for what doesn't parse */
    TEST_ASSERT_TRUE(proto_fw_cap_allows(PROTO_FW_CAP_NONE, NULL, "9.9.9"));
    TEST_ASSERT_TRUE(proto_fw_cap_allows(PROTO_FW_CAP_NONE, "0.6.2", "abc"));
    /* paired, its master's version not heard yet: nothing passes */
    TEST_ASSERT_FALSE(proto_fw_cap_allows(PROTO_FW_CAP_UNKNOWN, NULL, "0.6.1"));
    TEST_ASSERT_FALSE(proto_fw_cap_allows(PROTO_FW_CAP_UNKNOWN, "0.6.2", "0.6.1"));
    /* at most its master's version */
    TEST_ASSERT_TRUE(proto_fw_cap_allows(PROTO_FW_CAP_AT, "0.6.2", "0.6.2"));
    TEST_ASSERT_TRUE(proto_fw_cap_allows(PROTO_FW_CAP_AT, "0.6.2", "0.6.1"));
    TEST_ASSERT_TRUE(proto_fw_cap_allows(PROTO_FW_CAP_AT, "0.6.2", "v0.6.2"));
    TEST_ASSERT_TRUE(proto_fw_cap_allows(PROTO_FW_CAP_AT, "0.6.2", "0.6.2-rc.1"));
    TEST_ASSERT_TRUE(proto_fw_cap_allows(PROTO_FW_CAP_AT, "0.6.2", "0.6.2+local"));
    TEST_ASSERT_FALSE(proto_fw_cap_allows(PROTO_FW_CAP_AT, "0.6.2", "0.6.3"));
    TEST_ASSERT_FALSE(proto_fw_cap_allows(PROTO_FW_CAP_AT, "0.6.2", "0.7.0-rc.1"));
    TEST_ASSERT_FALSE(proto_fw_cap_allows(PROTO_FW_CAP_AT, "0.6.2-rc.1", "0.6.2"));
    /* what doesn't parse, on either side, never passes */
    TEST_ASSERT_FALSE(proto_fw_cap_allows(PROTO_FW_CAP_AT, "0.6.2", "abc123"));
    TEST_ASSERT_FALSE(proto_fw_cap_allows(PROTO_FW_CAP_AT, "0.6.2", NULL));
    TEST_ASSERT_FALSE(proto_fw_cap_allows(PROTO_FW_CAP_AT, "", "0.6.1"));
    TEST_ASSERT_FALSE(proto_fw_cap_allows(PROTO_FW_CAP_AT, NULL, "0.6.1"));
    /* a cap this firmware doesn't know is no cap it can pass */
    TEST_ASSERT_FALSE(proto_fw_cap_allows((proto_fw_cap_t)7, "0.6.2", "0.6.1"));
}

/* LINK-031: the master's own slot serves a keypad whose chip revision is within its range. */
static void test_chip_rev_range(void)
{
    TEST_ASSERT_TRUE(proto_fw_chip_rev_fits(100u, 0u, 0u));
    TEST_ASSERT_TRUE(proto_fw_chip_rev_fits(101u, 100u, 199u));
    TEST_ASSERT_TRUE(proto_fw_chip_rev_fits(100u, 100u, 199u));
    TEST_ASSERT_TRUE(proto_fw_chip_rev_fits(199u, 100u, 199u));
    TEST_ASSERT_FALSE(proto_fw_chip_rev_fits(99u, 100u, 199u));
    TEST_ASSERT_FALSE(proto_fw_chip_rev_fits(301u, 100u, 199u));
    TEST_ASSERT_FALSE(proto_fw_chip_rev_fits(101u, 300u, 399u)); /* a v3 image on v1 silicon */
    TEST_ASSERT_TRUE(proto_fw_chip_rev_fits(301u, 300u, 0xFFFFu)); /* no newest */
    TEST_ASSERT_TRUE(proto_fw_chip_rev_fits(0xFFFFu, 0u, 0xFFFFu));
}

/* An application image's first 288 bytes, as esptool lays them out. */
static uint8_t IMG[40960];

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8u) & 0xFFu);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v & 0xFFFFu);
    put16(p + 2, v >> 16u);
}

static void make_header(uint8_t *h, uint16_t chip, uint16_t min_rev, uint16_t max_rev,
                        const char *project, const char *version)
{
    memset(h, 0, PROTO_FW_HEADER_LEN);
    h[0] = 0xE9u;
    h[1] = 1u;
    put16(h + 12, chip);
    h[14] = (uint8_t)(min_rev / 100u);
    put16(h + 15, min_rev);
    put16(h + 17, max_rev);
    h[23] = 1u;
    put32(h + 32, 0xABCD5432u);
    memcpy(h + 48, version, strlen(version) < 32u ? strlen(version) : 32u);
    memcpy(h + 80, project, strlen(project) < 32u ? strlen(project) : 32u);
}

/* LINK-033 */
static void test_header_read(void)
{
    uint8_t h[PROTO_FW_HEADER_LEN];
    make_header(h, PROTO_FW_CHIP_ESP32P4, 100u, 199u, "alarm_crowpanel_p4-release", "0.7.0");
    proto_fw_header_t r;
    TEST_ASSERT_TRUE(proto_fw_header_read(h, sizeof(h), &r));
    TEST_ASSERT_TRUE(r.image_magic);
    TEST_ASSERT_TRUE(r.app_magic);
    TEST_ASSERT_EQUAL_UINT8(1u, r.segments);
    TEST_ASSERT_EQUAL_HEX16(0x0012u, r.chip_id);
    TEST_ASSERT_EQUAL_UINT16(100u, r.min_rev_full);
    TEST_ASSERT_EQUAL_UINT16(199u, r.max_rev_full);
    TEST_ASSERT_TRUE(r.hash_appended);
    TEST_ASSERT_EQUAL_STRING("0.7.0", r.version);
    TEST_ASSERT_EQUAL_STRING("alarm_crowpanel_p4-release", r.project);

    /* fields that fill all 32 bytes, unterminated, are read whole */
    memset(h + 48, 'v', 32u);
    memset(h + 80, 'p', 32u);
    TEST_ASSERT_TRUE(proto_fw_header_read(h, sizeof(h), &r));
    TEST_ASSERT_EQUAL_UINT(32u, (unsigned)strlen(r.version));
    TEST_ASSERT_EQUAL_UINT(32u, (unsigned)strlen(r.project));

    /* too short, or nothing: refused and zeroed */
    TEST_ASSERT_FALSE(proto_fw_header_read(h, PROTO_FW_HEADER_LEN - 1u, &r));
    TEST_ASSERT_FALSE(r.image_magic);
    TEST_ASSERT_EQUAL_STRING("", r.version);
    TEST_ASSERT_FALSE(proto_fw_header_read(NULL, PROTO_FW_HEADER_LEN, &r));
    TEST_ASSERT_FALSE(proto_fw_header_read(h, sizeof(h), NULL));

    /* not an image: read, and the judge says so */
    memset(h, 0xFF, sizeof(h));
    TEST_ASSERT_TRUE(proto_fw_header_read(h, sizeof(h), &r));
    TEST_ASSERT_FALSE(r.image_magic);
    TEST_ASSERT_FALSE(r.app_magic);
}

static proto_fw_hdr_verdict_t judge(const uint8_t *h, const proto_fw_self_t *me)
{
    proto_fw_header_t r;
    TEST_ASSERT_TRUE(proto_fw_header_read(h, PROTO_FW_HEADER_LEN, &r));
    return proto_fw_header_judge(&r, me);
}

/* LINK-033, OTA-014: the header is judged before any flash is written, in this order: magic,
 * chip, chip revision range, project, newer; then the cap (LINK-029). */
static void test_header_judge(void)
{
    const proto_fw_self_t v1 = {
        .chip_id = PROTO_FW_CHIP_ESP32P4,
        .rev_full = 101u,
        .project = "alarm_crowpanel_p4-release",
        .version = "0.6.2",
    };
    uint8_t h[PROTO_FW_HEADER_LEN];
    make_header(h, PROTO_FW_CHIP_ESP32P4, 100u, 199u, "alarm_crowpanel_p4-release", "0.7.0");
    TEST_ASSERT_EQUAL_INT(PROTO_FW_HDR_OK, judge(h, &v1));

    /* not an image: either magic */
    h[0] = 0xEAu;
    TEST_ASSERT_EQUAL_INT(PROTO_FW_HDR_NOT_AN_IMAGE, judge(h, &v1));
    h[0] = 0xE9u;
    h[32] = 0x33u;
    TEST_ASSERT_EQUAL_INT(PROTO_FW_HDR_NOT_AN_IMAGE, judge(h, &v1));
    TEST_ASSERT_EQUAL_INT(PROTO_FW_HDR_NOT_AN_IMAGE, proto_fw_header_judge(NULL, &v1));

    /* the C6's image, a v3 image on v1 silicon (refused before any write), and a v1 image on v3 */
    make_header(h, PROTO_FW_CHIP_ESP32C6, 0u, 99u, "alarm_crowpanel_p4-release", "0.7.0");
    TEST_ASSERT_EQUAL_INT(PROTO_FW_HDR_OTHER_CHIP, judge(h, &v1));
    make_header(h, PROTO_FW_CHIP_ESP32P4, 300u, 399u, "alarm_crowpanel_p4-release", "0.7.0");
    TEST_ASSERT_EQUAL_INT(PROTO_FW_HDR_OTHER_REVISION, judge(h, &v1));
    proto_fw_self_t v3 = v1;
    v3.rev_full = 301u;
    make_header(h, PROTO_FW_CHIP_ESP32P4, 100u, 199u, "alarm_crowpanel_p4-release", "0.7.0");
    TEST_ASSERT_EQUAL_INT(PROTO_FW_HDR_OTHER_REVISION, judge(h, &v3));
    make_header(h, PROTO_FW_CHIP_ESP32P4, 0u, 0u, "alarm_crowpanel_p4-release", "0.7.0");
    TEST_ASSERT_EQUAL_INT(PROTO_FW_HDR_OK, judge(h, &v3)); /* no range: any revision */

    /* the project (DEV-004): a release panel takes no development build; another board's never */
    make_header(h, PROTO_FW_CHIP_ESP32P4, 100u, 199u, "alarm_crowpanel_p4-dev", "0.7.0");
    TEST_ASSERT_EQUAL_INT(PROTO_FW_HDR_OTHER_PROJECT, judge(h, &v1));
    make_header(h, PROTO_FW_CHIP_ESP32P4, 100u, 199u, "alarm_crowpanel_10-release", "0.7.0");
    TEST_ASSERT_EQUAL_INT(PROTO_FW_HDR_OTHER_PROJECT, judge(h, &v1));
    proto_fw_self_t dev = v1;
    dev.project = "alarm_crowpanel_p4-dev";
    make_header(h, PROTO_FW_CHIP_ESP32P4, 100u, 199u, "alarm_crowpanel_p4-release", "0.7.0");
    TEST_ASSERT_EQUAL_INT(PROTO_FW_HDR_OK, judge(h, &dev));

    /* never older or equal unless a person forced it (OTA-014) */
    make_header(h, PROTO_FW_CHIP_ESP32P4, 100u, 199u, "alarm_crowpanel_p4-release", "0.6.2");
    TEST_ASSERT_EQUAL_INT(PROTO_FW_HDR_NOT_NEWER, judge(h, &v1));
    make_header(h, PROTO_FW_CHIP_ESP32P4, 100u, 199u, "alarm_crowpanel_p4-release", "0.6.1");
    TEST_ASSERT_EQUAL_INT(PROTO_FW_HDR_NOT_NEWER, judge(h, &v1));
    proto_fw_self_t forced = v1;
    forced.allow_older = true;
    TEST_ASSERT_EQUAL_INT(PROTO_FW_HDR_OK, judge(h, &forced));

    /* the order: a chip refused comes before an older version */
    make_header(h, PROTO_FW_CHIP_ESP32C6, 0u, 0u, "alarm_crowpanel_p4-release", "0.1.0");
    TEST_ASSERT_EQUAL_INT(PROTO_FW_HDR_OTHER_CHIP, judge(h, &v1));
    make_header(h, PROTO_FW_CHIP_ESP32P4, 300u, 399u, "alarm_crowpanel_p4-dev", "0.1.0");
    TEST_ASSERT_EQUAL_INT(PROTO_FW_HDR_OTHER_REVISION, judge(h, &v1));

    /* never past its master (LINK-029) */
    proto_fw_self_t capped = v1;
    capped.cap = PROTO_FW_CAP_AT;
    capped.cap_version = "0.7.0";
    make_header(h, PROTO_FW_CHIP_ESP32P4, 100u, 199u, "alarm_crowpanel_p4-release", "0.7.0");
    TEST_ASSERT_EQUAL_INT(PROTO_FW_HDR_OK, judge(h, &capped));
    make_header(h, PROTO_FW_CHIP_ESP32P4, 100u, 199u, "alarm_crowpanel_p4-release", "0.7.1");
    TEST_ASSERT_EQUAL_INT(PROTO_FW_HDR_PAST_CAP, judge(h, &capped));
    capped.cap = PROTO_FW_CAP_UNKNOWN;
    make_header(h, PROTO_FW_CHIP_ESP32P4, 100u, 199u, "alarm_crowpanel_p4-release", "0.6.3");
    TEST_ASSERT_EQUAL_INT(PROTO_FW_HDR_PAST_CAP, judge(h, &capped));
    capped.allow_older = true; /* only a cable goes past: forced or not */
    make_header(h, PROTO_FW_CHIP_ESP32P4, 100u, 199u, "alarm_crowpanel_p4-release", "0.6.1");
    TEST_ASSERT_EQUAL_INT(PROTO_FW_HDR_PAST_CAP, judge(h, &capped));
    /* a keypad newer than its master (it went back by cable) is never sent back: older is
     * refused first, and anything newer is past the cap */
    proto_fw_self_t ahead = v1;
    ahead.cap = PROTO_FW_CAP_AT;
    ahead.cap_version = "0.6.0";
    make_header(h, PROTO_FW_CHIP_ESP32P4, 100u, 199u, "alarm_crowpanel_p4-release", "0.6.0");
    TEST_ASSERT_EQUAL_INT(PROTO_FW_HDR_NOT_NEWER, judge(h, &ahead));
    make_header(h, PROTO_FW_CHIP_ESP32P4, 100u, 199u, "alarm_crowpanel_p4-release", "0.6.3");
    TEST_ASSERT_EQUAL_INT(PROTO_FW_HDR_PAST_CAP, judge(h, &ahead));
}

/* LINK-033: a refused header is a refusal (30-49), never a transfer failure the relay retries. */
static void test_header_why(void)
{
    TEST_ASSERT_EQUAL_INT(PROTO_FW_WHY_OK, proto_fw_header_why(PROTO_FW_HDR_OK));
    TEST_ASSERT_EQUAL_INT(PROTO_FW_WHY_SIGNATURE, proto_fw_header_why(PROTO_FW_HDR_NOT_AN_IMAGE));
    TEST_ASSERT_EQUAL_INT(PROTO_FW_WHY_CHIP, proto_fw_header_why(PROTO_FW_HDR_OTHER_CHIP));
    TEST_ASSERT_EQUAL_INT(PROTO_FW_WHY_CHIP, proto_fw_header_why(PROTO_FW_HDR_OTHER_REVISION));
    TEST_ASSERT_EQUAL_INT(PROTO_FW_WHY_PROJECT, proto_fw_header_why(PROTO_FW_HDR_OTHER_PROJECT));
    TEST_ASSERT_EQUAL_INT(PROTO_FW_WHY_NOT_NEWER, proto_fw_header_why(PROTO_FW_HDR_NOT_NEWER));
    TEST_ASSERT_EQUAL_INT(PROTO_FW_WHY_WRONG_VERSION, proto_fw_header_why(PROTO_FW_HDR_PAST_CAP));
    TEST_ASSERT_EQUAL_INT(PROTO_FW_WHY_SIGNATURE, proto_fw_header_why((proto_fw_hdr_verdict_t)99));
    for (int v = PROTO_FW_HDR_NOT_AN_IMAGE; v <= PROTO_FW_HDR_PAST_CAP; v++) {
        const unsigned why = (unsigned)proto_fw_header_why((proto_fw_hdr_verdict_t)v);
        TEST_ASSERT_TRUE(why >= 30u && why <= 49u);
    }
}

/* The slot read the walk uses: IMG, or a failing read past `s_readable`. */
static uint32_t s_readable;
static unsigned s_reads;

static bool read_img(void *ctx, uint32_t offset, void *out, size_t len)
{
    (void)ctx;
    s_reads++;
    if ((uint64_t)offset + len > s_readable) {
        return false;
    }
    memcpy(out, IMG + offset, len);
    return true;
}

/* Lays out an image in IMG: header, segments of these lengths (the first holds the application
 * description), the checksum padded to 16, the SHA-256 when hashed. Returns where it ends. */
static uint32_t make_image(uint16_t chip, const uint32_t *segs, unsigned n, bool hashed)
{
    memset(IMG, 0xFF, sizeof(IMG));
    make_header(IMG, chip, 100u, 199u, "alarm_crowpanel_p4-release", "0.7.0");
    IMG[1] = (uint8_t)n;
    IMG[23] = hashed ? 1u : 0u;
    uint32_t off = 24u;
    for (unsigned i = 0; i < n; i++) {
        put32(IMG + off, 0x40000000u + i);
        put32(IMG + off + 4u, segs[i]);
        if (i != 0u) {
            memset(IMG + off + 8u, 0x5A, segs[i]);
        }
        off += 8u + segs[i];
    }
    off = (off + 16u) & ~15u;
    IMG[off - 1u] = 0xC3u; /* the checksum */
    if (hashed) {
        memset(IMG + off, 0x11, 32u);
        off += 32u;
    }
    s_readable = sizeof(IMG);
    return off;
}

static uint32_t up4k(uint32_t v)
{
    return (v + 4095u) & ~4095u;
}

/* LINK-031, LINK-032: the exact length a master relays from its own slot (docs/features/11, image
 * sources: the header walk made for the P4). */
static void test_image_size(void)
{
    const uint32_t segs[] = {512u, 3000u, 77u};
    const uint32_t limit = sizeof(IMG);

    /* hashed and signed: up to the end of the 4 KB signature sector */
    uint32_t end = make_image(PROTO_FW_CHIP_ESP32P4, segs, 3u, true);
    TEST_ASSERT_EQUAL_UINT32(24u + 3u * 8u + 512u + 3000u + 77u + 11u + 32u, end);
    IMG[up4k(end)] = 0xE7u;
    TEST_ASSERT_EQUAL_UINT32(up4k(end) + 4096u,
                             proto_fw_image_size(read_img, NULL, limit, PROTO_FW_CHIP_ESP32P4));

    /* no signature sector: the image ends with its hash */
    const uint16_t p4 = PROTO_FW_CHIP_ESP32P4;
    end = make_image(p4, segs, 3u, true);
    TEST_ASSERT_EQUAL_UINT32(end, proto_fw_image_size(read_img, NULL, limit, p4));

    /* no hash: the checksum's 16 bytes end it */
    end = make_image(p4, segs, 3u, false);
    TEST_ASSERT_EQUAL_UINT32(0u, end % 16u);
    TEST_ASSERT_EQUAL_UINT32(end, proto_fw_image_size(read_img, NULL, limit, p4));
    IMG[up4k(end)] = 0xE7u;
    TEST_ASSERT_EQUAL_UINT32(up4k(end) + 4096u,
                             proto_fw_image_size(read_img, NULL, limit, PROTO_FW_CHIP_ESP32P4));

    /* an image that ends on a 4 KB boundary (padded, as a signed build is): the sector follows
     * at once */
    const uint32_t padded[] = {512u, 4096u - 24u - 16u - 512u - 1u - 32u - 15u};
    end = make_image(PROTO_FW_CHIP_ESP32P4, padded, 2u, true);
    TEST_ASSERT_EQUAL_UINT32(4096u, end);
    IMG[end] = 0xE7u;
    TEST_ASSERT_EQUAL_UINT32(8192u, proto_fw_image_size(read_img, NULL, limit, p4));

    /* a signature sector that doesn't fit the slot isn't counted, nor read */
    end = make_image(PROTO_FW_CHIP_ESP32P4, segs, 3u, true);
    IMG[up4k(end)] = 0xE7u;
    TEST_ASSERT_EQUAL_UINT32(end, proto_fw_image_size(read_img, NULL, up4k(end) + 4095u,
                                                       PROTO_FW_CHIP_ESP32P4));
    /* nor one the slot can't read */
    s_readable = up4k(end);
    TEST_ASSERT_EQUAL_UINT32(end, proto_fw_image_size(read_img, NULL, limit, p4));

    /* not an image for this chip */
    make_image(PROTO_FW_CHIP_ESP32P4, segs, 3u, true);
    TEST_ASSERT_EQUAL_UINT32(0u, proto_fw_image_size(read_img, NULL, limit, PROTO_FW_CHIP_ESP32C6));
    IMG[0] = 0xE8u;
    TEST_ASSERT_EQUAL_UINT32(0u, proto_fw_image_size(read_img, NULL, limit, PROTO_FW_CHIP_ESP32P4));
    make_image(PROTO_FW_CHIP_ESP32P4, segs, 3u, true);
    IMG[32] = 0u; /* no application description */
    TEST_ASSERT_EQUAL_UINT32(0u, proto_fw_image_size(read_img, NULL, limit, PROTO_FW_CHIP_ESP32P4));

    /* no segments, too many, or one running past the slot (also near 4 GB) */
    make_image(PROTO_FW_CHIP_ESP32P4, segs, 3u, true);
    IMG[1] = 0u;
    TEST_ASSERT_EQUAL_UINT32(0u, proto_fw_image_size(read_img, NULL, limit, PROTO_FW_CHIP_ESP32P4));
    IMG[1] = 17u;
    TEST_ASSERT_EQUAL_UINT32(0u, proto_fw_image_size(read_img, NULL, limit, PROTO_FW_CHIP_ESP32P4));
    make_image(PROTO_FW_CHIP_ESP32P4, segs, 3u, true);
    put32(IMG + 24u + 8u + 512u + 4u, limit);
    TEST_ASSERT_EQUAL_UINT32(0u, proto_fw_image_size(read_img, NULL, limit, PROTO_FW_CHIP_ESP32P4));
    put32(IMG + 24u + 8u + 512u + 4u, 0xFFFFFFF0u);
    TEST_ASSERT_EQUAL_UINT32(0u, proto_fw_image_size(read_img, NULL, limit, PROTO_FW_CHIP_ESP32P4));
    /* the image itself longer than the slot */
    end = make_image(PROTO_FW_CHIP_ESP32P4, segs, 3u, true);
    TEST_ASSERT_EQUAL_UINT32(0u, proto_fw_image_size(read_img, NULL, end - 1u, p4));
    TEST_ASSERT_EQUAL_UINT32(end, proto_fw_image_size(read_img, NULL, end, p4));

    /* a slot that can't be read, a slot too small for a header, no reader */
    s_readable = 100u;
    TEST_ASSERT_EQUAL_UINT32(0u, proto_fw_image_size(read_img, NULL, limit, PROTO_FW_CHIP_ESP32P4));
    s_readable = sizeof(IMG);
    TEST_ASSERT_EQUAL_UINT32(0u, proto_fw_image_size(read_img, NULL, PROTO_FW_HEADER_LEN - 1u,
                                                     PROTO_FW_CHIP_ESP32P4));
    TEST_ASSERT_EQUAL_UINT32(0u, proto_fw_image_size(NULL, NULL, limit, PROTO_FW_CHIP_ESP32P4));

    /* it reads headers only, never the segments' data */
    make_image(PROTO_FW_CHIP_ESP32P4, segs, 3u, true);
    s_reads = 0u;
    TEST_ASSERT_NOT_EQUAL(0u, proto_fw_image_size(read_img, NULL, limit, PROTO_FW_CHIP_ESP32P4));
    TEST_ASSERT_EQUAL_UINT(2u + 3u + 1u, s_reads);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_signed_profiles);
    RUN_TEST(test_version_parse);
    RUN_TEST(test_version_order);
    RUN_TEST(test_version_upgrade_policy);
    RUN_TEST(test_manifest_pick);
    RUN_TEST(test_manifest_urls_and_errors);
    RUN_TEST(test_manifest_languages);
    RUN_TEST(test_manifest_assets);
    RUN_TEST(test_install_policy);
    RUN_TEST(test_check_retry_schedule);
    RUN_TEST(test_selftest_part_verdicts);
    RUN_TEST(test_selftest_steps);
    RUN_TEST(test_selftest_keypads_come_back);
    RUN_TEST(test_base64);
    RUN_TEST(test_upload_line);
    RUN_TEST(test_update_state);
    RUN_TEST(test_update_discovery);
    RUN_TEST(test_why_numbers_frozen);
    RUN_TEST(test_version_cap);
    RUN_TEST(test_chip_rev_range);
    RUN_TEST(test_header_read);
    RUN_TEST(test_header_judge);
    RUN_TEST(test_header_why);
    RUN_TEST(test_image_size);
    return UNITY_END();
}
