/* ZW-033 (docs/features/36): which firmware the Z-Wave JS firmware update service's answer offers a
 * device - exactly that device, never a downgrade or the same version, stable unless beta was asked
 * for, the panel's region or none, and only a file it can verify (https, SHA-256). The answers are
 * shaped like the service's v4 answer (github.com/zwave-js/firmware-updates, checked 2026-09-27);
 * the device and URLs are fictional. */
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "zwave_zones/zw_fwcat.h"

void setUp(void) {}
void tearDown(void) {}

#define SHA_A "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define SHA_B "sha256:fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"

static const zwfc_device_t DEV = {
    .manufacturer = 0x0123u, .product_type = 0x0456u, .product_id = 0x0789u, .fw_major = 1u,
    .fw_minor = 18u};

static char BUF[4096];

/* One device's entry, its updates given as JSON text. */
static const char *answer_for(const char *ids, const char *updates)
{
    snprintf(BUF, sizeof BUF,
             "[{%s,\"firmwareVersion\":\"1.18\",\"updates\":[%s]}]", ids, updates);
    return BUF;
}
#define MY_IDS "\"manufacturerId\":\"0x0123\",\"productType\":\"0x0456\",\"productId\":\"0x0789\""

static const char *update(const char *version, const char *channel, bool downgrade,
                          const char *extra, const char *file)
{
    static char u[4][1024];
    static unsigned n;
    char *out = u[n++ % 4u];
    snprintf(out, 1024,
             "{\"version\":\"%s\",\"changelog\":\"Fixes.\",\"channel\":\"%s\",\"downgrade\":%s%s,"
             "\"files\":[%s]}",
             version, channel, downgrade ? "true" : "false", extra, file);
    return out;
}
#define FILE_OK(v) "{\"target\":0,\"url\":\"https://example.com/fw-" v ".gbl\",\"integrity\":\"" SHA_A "\"}"

static zwfc_verdict_t best(const char *json, const char *region, bool beta, zwfc_update_t *u)
{
    return zwfc_best(json, strlen(json), &DEV, region, beta, u);
}

/* ZW-033: the request names exactly the device and its installed version. */
static void test_request_body(void)
{
    char out[256];
    size_t n = zwfc_request(&DEV, "usa", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING(
        "{\"region\":\"usa\",\"devices\":[{\"manufacturerId\":\"0x0123\",\"productType\":\"0x0456\","
        "\"productId\":\"0x0789\",\"firmwareVersion\":\"1.18\"}]}",
        out);
    TEST_ASSERT_EQUAL_UINT(strlen(out), n);
    zwfc_device_t sdk = DEV;
    sdk.has_sdk = true;
    sdk.sdk_major = 7u;
    sdk.sdk_minor = 18u;
    sdk.sdk_patch = 3u;
    n = zwfc_request(&sdk, NULL, out, sizeof out);
    TEST_ASSERT_NOT_NULL(strstr(out, "\"sdkVersion\":\"7.18.3\""));
    TEST_ASSERT_NULL(strstr(out, "region"));
    TEST_ASSERT_EQUAL_UINT(0u, zwfc_request(&DEV, "usa", out, 40u)); /* doesn't fit */
    TEST_ASSERT_EQUAL_STRING("usa", zwfc_region(0x01u));
    TEST_ASSERT_EQUAL_STRING("europe", zwfc_region(0x00u));
    TEST_ASSERT_NULL(zwfc_region(0x7Fu));
}

/* ZW-033: the highest stable newer version, its file and digest. */
static void test_highest_stable_newer(void)
{
    char ups[2048];
    snprintf(ups, sizeof ups, "%s,%s,%s", update("1.20", "stable", false, "", FILE_OK("120")),
             update("1.22", "stable", false, "", FILE_OK("122")),
             update("1.21", "stable", false, "", FILE_OK("121")));
    zwfc_update_t u;
    TEST_ASSERT_EQUAL(ZWFC_UPDATE, best(answer_for(MY_IDS, ups), "usa", false, &u));
    TEST_ASSERT_EQUAL_STRING("1.22", u.version);
    TEST_ASSERT_EQUAL_UINT16(1u, u.major);
    TEST_ASSERT_EQUAL_UINT16(22u, u.minor);
    TEST_ASSERT_EQUAL_UINT8(1u, u.file_count);
    TEST_ASSERT_EQUAL_STRING("https://example.com/fw-122.gbl", u.files[0].url);
    TEST_ASSERT_EQUAL_HEX8(0x01u, u.files[0].sha256[0]);
    TEST_ASSERT_EQUAL_HEX8(0xEFu, u.files[0].sha256[31]);
    TEST_ASSERT_EQUAL_STRING("Fixes.", u.changelog);
    TEST_ASSERT_FALSE(u.beta);
}

/* ZW-033: beta only when asked for; never a downgrade or the same version, by the service's flag
 * or by the numbers (1.9 is older than 1.18). */
static void test_beta_and_downgrades(void)
{
    zwfc_update_t u;
    const char *beta = answer_for(MY_IDS, update("1.30", "beta", false, "", FILE_OK("130")));
    TEST_ASSERT_EQUAL(ZWFC_UP_TO_DATE, best(beta, NULL, false, &u));
    TEST_ASSERT_EQUAL(ZWFC_UPDATE, best(beta, NULL, true, &u));
    TEST_ASSERT_TRUE(u.beta);

    TEST_ASSERT_EQUAL(ZWFC_UP_TO_DATE,
                      best(answer_for(MY_IDS, update("1.30", "stable", true, "", FILE_OK("130"))),
                           NULL, false, &u));
    TEST_ASSERT_EQUAL(ZWFC_UP_TO_DATE,
                      best(answer_for(MY_IDS, update("1.9", "stable", false, "", FILE_OK("19"))),
                           NULL, false, &u));
    TEST_ASSERT_EQUAL(ZWFC_UP_TO_DATE,
                      best(answer_for(MY_IDS, update("1.18", "stable", false, "", FILE_OK("118"))),
                           NULL, false, &u));
    /* its normalized version decides when given: "1.18.1" is newer than 1.18 */
    TEST_ASSERT_EQUAL(
        ZWFC_UPDATE,
        best(answer_for(MY_IDS, update("1.18.1", "stable", false, ",\"normalizedVersion\":\"1.18.1\"",
                                       FILE_OK("1181"))),
             NULL, false, &u));
    TEST_ASSERT_EQUAL_UINT16(1u, u.patch);
    /* no downgrade flag at all: not offered */
    snprintf(BUF, sizeof BUF,
             "[{" MY_IDS ",\"updates\":[{\"version\":\"1.30\",\"channel\":\"stable\",\"files\":["
             FILE_OK("130") "]}]}]");
    TEST_ASSERT_EQUAL(ZWFC_UP_TO_DATE, best(BUF, NULL, false, &u));
    /* an unknown channel isn't stable */
    TEST_ASSERT_EQUAL(ZWFC_UP_TO_DATE,
                      best(answer_for(MY_IDS, update("1.30", "nightly", false, "", FILE_OK("130"))),
                           NULL, true, &u));
}

/* ZW-033: the device must be exactly the one asked about; another product's updates are never
 * taken for it. */
static void test_exact_device(void)
{
    zwfc_update_t u;
    const char *other = answer_for(
        "\"manufacturerId\":\"0x0123\",\"productType\":\"0x0456\",\"productId\":\"0x078A\"",
        update("1.30", "stable", false, "", FILE_OK("130")));
    TEST_ASSERT_EQUAL(ZWFC_UNKNOWN_DEVICE, best(other, NULL, false, &u));
    TEST_ASSERT_EQUAL_STRING("", u.version);
    TEST_ASSERT_EQUAL(ZWFC_UNKNOWN_DEVICE, best("[]", NULL, false, &u));
    /* upper case hex is the same device */
    TEST_ASSERT_EQUAL(
        ZWFC_UPDATE,
        best(answer_for("\"manufacturerId\":\"0X0123\",\"productType\":\"0x0456\",\"productId\":"
                        "\"0x0789\"",
                        update("1.30", "stable", false, "", FILE_OK("130"))),
             NULL, false, &u));
    /* both entries: only its own counts */
    char two[3000];
    snprintf(two, sizeof two,
             "[{\"manufacturerId\":\"0x0999\",\"productType\":\"0x0456\",\"productId\":\"0x0789\","
             "\"updates\":[%s]},{" MY_IDS ",\"updates\":[%s]}]",
             update("9.0", "stable", false, "", FILE_OK("90")),
             update("1.20", "stable", false, "", FILE_OK("120")));
    TEST_ASSERT_EQUAL(ZWFC_UPDATE, best(two, NULL, false, &u));
    TEST_ASSERT_EQUAL_STRING("1.20", u.version);
    /* known, nothing newer */
    TEST_ASSERT_EQUAL(ZWFC_UP_TO_DATE, best(answer_for(MY_IDS, ""), NULL, false, &u));
}

/* ZW-033: region - none, or the panel's; a regional update without a panel region isn't taken. */
static void test_region(void)
{
    zwfc_update_t u;
    const char *eu =
        answer_for(MY_IDS, update("1.30", "stable", false, ",\"region\":\"europe\"", FILE_OK("130")));
    TEST_ASSERT_EQUAL(ZWFC_UP_TO_DATE, best(eu, "usa", false, &u));
    TEST_ASSERT_EQUAL(ZWFC_UP_TO_DATE, best(eu, NULL, false, &u));
    TEST_ASSERT_EQUAL(ZWFC_UPDATE, best(eu, "europe", false, &u));
}

/* ZW-033: only a file it can verify - https, a SHA-256 of 64 hex digits, a target; one bad file
 * spoils its update; target 0 (the device's own firmware) must be among them. */
static void test_files_verified(void)
{
    zwfc_update_t u;
    static const char *const bad[] = {
        "{\"target\":0,\"url\":\"http://example.com/fw.gbl\",\"integrity\":\"" SHA_A "\"}",
        "{\"target\":0,\"url\":\"https://example.com/fw.gbl\",\"integrity\":\"sha1:0123\"}",
        "{\"target\":0,\"url\":\"https://example.com/fw.gbl\",\"integrity\":\"sha256:0123\"}",
        "{\"target\":0,\"url\":\"https://example.com/fw.gbl\",\"integrity\":"
        "\"sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg\"}",
        "{\"url\":\"https://example.com/fw.gbl\",\"integrity\":\"" SHA_A "\"}",
        "{\"target\":1,\"url\":\"https://example.com/fw.gbl\",\"integrity\":\"" SHA_A "\"}",
        "{\"target\":0,\"url\":\"https://example.com/fw.gbl\",\"integrity\":\"" SHA_A "\"},"
        "{\"target\":1,\"url\":\"ftp://example.com/fw1.gbl\",\"integrity\":\"" SHA_B "\"}",
        "",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(
            ZWFC_UP_TO_DATE, best(answer_for(MY_IDS, update("1.30", "stable", false, "", bad[i])),
                                  NULL, false, &u),
            bad[i]);
    }
    /* two good files: both kept */
    TEST_ASSERT_EQUAL(
        ZWFC_UPDATE,
        best(answer_for(MY_IDS,
                        update("1.30", "stable", false, "",
                               FILE_OK("130") ",{\"target\":1,\"url\":\"https://example.com/"
                                              "fw1.gbl\",\"integrity\":\"" SHA_B "\"}")),
             NULL, false, &u));
    TEST_ASSERT_EQUAL_UINT8(2u, u.file_count);
    TEST_ASSERT_EQUAL_UINT8(1u, u.files[1].target);
}

/* ZW-033: malformed answers are refused whole, never half-read; a long changelog is left out. */
static void test_malformed(void)
{
    zwfc_update_t u;
    static const char *const bad[] = {
        "", "{}", "[", "[{\"manufacturerId\":\"0x0123\"", "[1]", "[{" MY_IDS ",\"updates\":{}}]",
        "[{" MY_IDS ",\"updates\":[1]}]", "[] trailing",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(ZWFC_BAD_ANSWER, best(bad[i], NULL, false, &u), bad[i]);
    }
    /* odd ids and versions: not the device, not a version */
    TEST_ASSERT_EQUAL(
        ZWFC_UNKNOWN_DEVICE,
        best(answer_for("\"manufacturerId\":\"0x00123\",\"productType\":\"0x0456\",\"productId\":"
                        "\"0x0789\"",
                        update("1.30", "stable", false, "", FILE_OK("130"))),
             NULL, false, &u));
    TEST_ASSERT_EQUAL(ZWFC_UP_TO_DATE,
                      best(answer_for(MY_IDS, update("1.30b", "stable", false, "", FILE_OK("130"))),
                           NULL, false, &u));
    TEST_ASSERT_EQUAL(ZWFC_UP_TO_DATE,
                      best(answer_for(MY_IDS, update("1..3", "stable", false, "", FILE_OK("130"))),
                           NULL, false, &u));
    TEST_ASSERT_EQUAL(ZWFC_UP_TO_DATE,
                      best(answer_for(MY_IDS, update("99999.1", "stable", false, "", FILE_OK("130"))),
                           NULL, false, &u));
    char log[400];
    memset(log, 'x', sizeof log - 1u);
    log[sizeof log - 1u] = '\0';
    char up[1400];
    snprintf(up, sizeof up,
             "{\"version\":\"1.30\",\"changelog\":\"%s\",\"channel\":\"stable\",\"downgrade\":false,"
             "\"files\":[" FILE_OK("130") "]}",
             log);
    TEST_ASSERT_EQUAL(ZWFC_UPDATE, best(answer_for(MY_IDS, up), NULL, false, &u));
    TEST_ASSERT_EQUAL_STRING("", u.changelog);
    TEST_ASSERT_TRUE(u.changelog_cut);
}

/* ZW-033, ZW-023: an answer damaged anywhere - bytes changed, cut short - is read to the end or
 * refused, never beyond its length; anything offered from it is still newer, stable, https with a
 * digest. */
static void test_damaged_answers(void)
{
    char ups[2048];
    snprintf(ups, sizeof ups, "%s,%s", update("1.20", "stable", false, "", FILE_OK("120")),
             update("1.9", "beta", false, ",\"region\":\"usa\"", FILE_OK("19")));
    const char *good = answer_for(MY_IDS, ups);
    char base[4096];
    const size_t n = strlen(good);
    memcpy(base, good, n);
    uint64_t seed = 0x5EEDu;
    for (unsigned iter = 0; iter < 20000u; iter++) {
        char buf[4096];
        memcpy(buf, base, n);
        size_t len = n;
        for (unsigned k = 0; k < 1u + iter % 4u; k++) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            const size_t at = (size_t)(seed >> 33) % n;
            buf[at] = (char)((seed >> 20) & 0x7Fu);
        }
        if (iter % 5u == 0u) {
            len = (size_t)(seed >> 40) % n; /* cut short */
        }
        zwfc_update_t u;
        const zwfc_verdict_t v = zwfc_best(buf, len, &DEV, "usa", false, &u);
        if (v == ZWFC_UPDATE) {
            TEST_ASSERT_TRUE(u.major > 1u || (u.major == 1u && u.minor > 18u));
            TEST_ASSERT_FALSE(u.beta);
            TEST_ASSERT_EQUAL_INT(0, strncmp(u.files[0].url, "https://", 8u));
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_request_body);
    RUN_TEST(test_highest_stable_newer);
    RUN_TEST(test_beta_and_downgrades);
    RUN_TEST(test_exact_device);
    RUN_TEST(test_region);
    RUN_TEST(test_files_verified);
    RUN_TEST(test_malformed);
    RUN_TEST(test_damaged_answers);
    return UNITY_END();
}
