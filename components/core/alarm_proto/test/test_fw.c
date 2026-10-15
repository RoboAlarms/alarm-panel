/*
 * test_fw.c - firmware update helpers (alarm_proto fw.c): versions, the release manifest, the
 * install policy, the serial upload framing and the Home Assistant update entity, plus the
 * panel's retry schedule for failed release checks.
 * Requirements: OTA-002, OTA-004, OTA-010, OTA-013, OTA-014, OTA-015 (docs/features/13)
 */
#include <stdio.h>
#include <string.h>

#include "alarm_proto/proto_fw.h"
#include "unity.h"

/* The retry schedule and the signed-profile rule the update task uses, portable parts of
 * alarm_proto since docs/features/33 step 8. */
#include "alarm_proto/ota_retry.h"
#include "alarm_proto/ota_profile.h"

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
    RUN_TEST(test_base64);
    RUN_TEST(test_upload_line);
    RUN_TEST(test_update_state);
    RUN_TEST(test_update_discovery);
    return UNITY_END();
}
