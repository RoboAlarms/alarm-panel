/*
 * test_fwsrv.c - talking to a firmware server (alarm_proto fwsrv.c): the server's domain and
 * address, channels, the check request and answer, language packs, signing keys, the signature
 * block's key digest and the trust record.
 * Requirements: OTA-017, OTA-018, OTA-019, OTA-020, OTA-024, OTA-025 (docs/features/13)
 */
#include <stdio.h>
#include <string.h>

#include "alarm/alarm_codec.h"
#include "alarm_proto/proto_fwsrv.h"
#include "fwsrv_sigblock_fixture.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define BASE "https://firmware.example.net"
#define SHA_A "3f9a1c2b77d0e4150011223344556677889900aabbccddeeff00112233445566"
#define SHA_B "6be04a1f00000000000000000000000000000000000000000000000000000001"
#define SHA_K "4d844cc724ee67890889affb5b71780181eb259c3f47dfa43b33c5a27555393d"

static alarm_result_t check(const char *json, proto_fwsrv_answer_t *a)
{
    return proto_fwsrv_check_answer(json, strlen(json), BASE, a);
}

/* ---- the server's domain (OTA-017) ------------------------------------------------------- */

/* OTA-017 */
static void test_domain_accepts_and_normalises(void)
{
    char out[PROTO_FWSRV_DOMAIN_MAX];
    TEST_ASSERT_TRUE(proto_fwsrv_domain("example.net", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("example.net", out);
    TEST_ASSERT_TRUE(proto_fwsrv_domain("builds.example.co.uk", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("builds.example.co.uk", out);
    TEST_ASSERT_TRUE(proto_fwsrv_domain("  HTTPS://Example.NET/ ", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("example.net", out);
    TEST_ASSERT_TRUE(proto_fwsrv_domain("http://my-builds.example.org", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("my-builds.example.org", out);
    TEST_ASSERT_TRUE(proto_fwsrv_domain("\texample.net\r\n", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("example.net", out);
}

/* OTA-017 */
static void test_domain_refuses(void)
{
    char out[PROTO_FWSRV_DOMAIN_MAX];
    const char *bad[] = {"localhost", "192.0.2.7", "example", "exa mple.net", "a..b",
                         "example.net/path", "example.net:8080", "user@example.net", "",
                         "   ", "https://", ".example.net", "example.net.", "-a.example.net",
                         "a-.example.net", "example.n3t", "example.net//", "ftp://example.net",
                         "exämple.net", "a_b.example.net", "https://https://example.net"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        memcpy(out, "junk", 5);
        TEST_ASSERT_FALSE_MESSAGE(proto_fwsrv_domain(bad[i], out, sizeof(out)), bad[i]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("", out, bad[i]);
    }
    TEST_ASSERT_FALSE(proto_fwsrv_domain(NULL, out, sizeof(out)));

    /* label lengths: 63 passes, 64 doesn't */
    char typed[160];
    memset(typed, 'a', 63);
    memcpy(typed + 63, ".net", 5);
    TEST_ASSERT_TRUE(proto_fwsrv_domain(typed, out, sizeof(out)));
    memset(typed, 'a', 64);
    memcpy(typed + 64, ".net", 5);
    TEST_ASSERT_FALSE(proto_fwsrv_domain(typed, out, sizeof(out)));

    /* total length: 95 passes, 96 doesn't (PROTO_FWSRV_DOMAIN_MAX - 1) */
    memset(typed, 'a', 60);
    typed[60] = '.';
    memset(typed + 61, 'b', 30);
    memcpy(typed + 91, ".net", 5); /* 95 */
    TEST_ASSERT_TRUE(proto_fwsrv_domain(typed, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(95u, strlen(out));
    memset(typed + 61, 'b', 31);
    memcpy(typed + 92, ".net", 5); /* 96 */
    TEST_ASSERT_FALSE(proto_fwsrv_domain(typed, out, sizeof(out)));

    /* an output too small */
    char small[8];
    TEST_ASSERT_FALSE(proto_fwsrv_domain("example.net", small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("", small);
}

/* OTA-017 */
static void test_base_url(void)
{
    char out[PROTO_FWSRV_BASE_MAX];
    TEST_ASSERT_TRUE(proto_fwsrv_base_url("example.net", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://firmware.example.net", out);
    TEST_ASSERT_TRUE(proto_fwsrv_base_url("http://192.0.2.10:8080", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("http://192.0.2.10:8080", out);
    TEST_ASSERT_TRUE(proto_fwsrv_base_url("http://192.0.2.10:8080/", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("http://192.0.2.10:8080", out);
    TEST_ASSERT_TRUE(proto_fwsrv_base_url("https://builds.example.org", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://builds.example.org", out);

    const char *bad[] = {"", "localhost", "192.0.2.10", "ftp://192.0.2.10", "http://",
                         "http://192.0.2.10:0", "http://192.0.2.10:65536", "http://192.0.2.10:",
                         "http://192.0.2.10/api", "http://user@192.0.2.10", "example.net/x"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(proto_fwsrv_base_url(bad[i], out, sizeof(out)), bad[i]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("", out, bad[i]);
    }
    TEST_ASSERT_FALSE(proto_fwsrv_base_url(NULL, out, sizeof(out)));

    /* the longest domain still fits PROTO_FWSRV_BASE_MAX */
    char typed[PROTO_FWSRV_DOMAIN_MAX];
    memset(typed, 'a', 60);
    typed[60] = '.';
    memset(typed + 61, 'b', 30);
    memcpy(typed + 91, ".net", 5);
    TEST_ASSERT_TRUE(proto_fwsrv_base_url(typed, out, sizeof(out)));
    char tiny[20];
    TEST_ASSERT_FALSE(proto_fwsrv_base_url("example.net", tiny, sizeof(tiny)));
}

/* OTA-019 */
static void test_channels(void)
{
    TEST_ASSERT_TRUE(proto_fwsrv_channel_valid("stable"));
    TEST_ASSERT_TRUE(proto_fwsrv_channel_valid("beta"));
    TEST_ASSERT_TRUE(proto_fwsrv_channel_valid("alpha"));
    TEST_ASSERT_TRUE(proto_fwsrv_channel_valid("dev"));
    TEST_ASSERT_FALSE(proto_fwsrv_channel_valid("Stable"));
    TEST_ASSERT_FALSE(proto_fwsrv_channel_valid("nightly"));
    TEST_ASSERT_FALSE(proto_fwsrv_channel_valid(""));
    TEST_ASSERT_FALSE(proto_fwsrv_channel_valid("stable "));
    TEST_ASSERT_FALSE(proto_fwsrv_channel_valid(NULL));
}

/* ---- the check (OTA-018) ----------------------------------------------------------------- */

/* OTA-018, OTA-019 */
static void test_check_request(void)
{
    static const char *const per[] = {"cc1101", "zac93"};
    static const char *const lang[] = {"es-US"};
    proto_fwsrv_facts_t f = {
        .channel = "alpha", .device = "crowpanel_dhe04107d", .board_rev = "1.1",
        .chip_rev = "v3", .project = "alarm_crowpanel_p4-release",
        .version = "0.8.0-alpha.2", .peripherals = per, .peripheral_count = 2,
        .languages = lang, .language_count = 1,
    };
    char buf[512];
    pj_writer_t w;
    pj_init(&w, buf, sizeof(buf));
    proto_fwsrv_check_request(&w, &f);
    TEST_ASSERT_NOT_EQUAL(0, pj_finish(&w));
    TEST_ASSERT_EQUAL_STRING(
        "{\"channel\":\"alpha\",\"device\":\"crowpanel_dhe04107d\",\"board_rev\":\"1.1\","
        "\"chip_rev\":\"v3\",\"project\":\"alarm_crowpanel_p4-release\","
        "\"version\":\"0.8.0-alpha.2\",\"peripherals\":[\"cc1101\",\"zac93\"],"
        "\"languages\":[\"es-US\"]}",
        buf);

    /* no channel is stable; no board_rev is left out; no lists are empty lists */
    proto_fwsrv_facts_t g = {
        .channel = "", .device = "crowpanel_dhe04107d", .board_rev = NULL, .chip_rev = "v1",
        .project = "p", .version = "0.8.0",
    };
    pj_init(&w, buf, sizeof(buf));
    proto_fwsrv_check_request(&w, &g);
    TEST_ASSERT_NOT_EQUAL(0, pj_finish(&w));
    TEST_ASSERT_EQUAL_STRING(
        "{\"channel\":\"stable\",\"device\":\"crowpanel_dhe04107d\",\"chip_rev\":\"v1\","
        "\"project\":\"p\",\"version\":\"0.8.0\",\"peripherals\":[],\"languages\":[]}",
        buf);
    g.channel = NULL;
    g.board_rev = "";
    pj_init(&w, buf, sizeof(buf));
    proto_fwsrv_check_request(&w, &g);
    TEST_ASSERT_NOT_EQUAL(0, pj_finish(&w));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"channel\":\"stable\""));
    TEST_ASSERT_NULL(strstr(buf, "board_rev"));

    /* a buffer too small is reported, not cut */
    char tiny[40];
    pj_init(&w, tiny, sizeof(tiny));
    proto_fwsrv_check_request(&w, &f);
    TEST_ASSERT_EQUAL(0, pj_finish(&w));
}

static const char UPDATE_ANSWER[] =
    "{\"installed\":{\"version\":\"0.8.0-alpha.2\",\"channel\":\"alpha\",\"withdrawn\":false},"
    "\"update\":true,\"version\":\"0.8.0\",\"channel\":\"stable\",\"critical\":false,"
    "\"notes_url\":\"https://firmware.example.net/releases/0.8.0\","
    "\"image\":{\"url\":\"https://firmware.example.net/dl/0.8.0/x-v3-0.8.0-3f9a1c2b.bin\","
    "\"size\":2105344,\"sha256\":\"" SHA_A "\","
    "\"signing_key\":{\"name\":\"RoboAlarms release\",\"sha256\":\"" SHA_K "\"}},"
    "\"languages\":[{\"locale\":\"es-US\","
    "\"url\":\"https://firmware.example.net/dl/0.8.0/es-US-0.8.0-6be04a1f.robolang\","
    "\"size\":155648,\"sha256\":\"" SHA_B "\"}]}";

/* OTA-018, OTA-020 */
static void test_check_answer_update(void)
{
    proto_fwsrv_answer_t a;
    TEST_ASSERT_EQUAL(ALARM_OK, check(UPDATE_ANSWER, &a));
    TEST_ASSERT_EQUAL(PROTO_FWSRV_UPDATE, a.reason);
    TEST_ASSERT_EQUAL_STRING("alpha", a.installed_channel);
    TEST_ASSERT_FALSE(a.installed_withdrawn);
    TEST_ASSERT_EQUAL_STRING("stable", a.channel);
    TEST_ASSERT_FALSE(a.critical);
    TEST_ASSERT_EQUAL_STRING("0.8.0", a.image.version);
    TEST_ASSERT_EQUAL_STRING("https://firmware.example.net/dl/0.8.0/x-v3-0.8.0-3f9a1c2b.bin",
                             a.image.url);
    TEST_ASSERT_EQUAL_STRING("https://firmware.example.net/releases/0.8.0", a.image.notes_url);
    TEST_ASSERT_EQUAL_UINT32(2105344u, a.image.size);
    TEST_ASSERT_TRUE(a.image.has_sha256);
    TEST_ASSERT_EQUAL_HEX8(0x3f, a.image.sha256[0]);
    TEST_ASSERT_EQUAL_HEX8(0x66, a.image.sha256[31]);
    TEST_ASSERT_TRUE(a.has_key);
    TEST_ASSERT_EQUAL_STRING("RoboAlarms release", a.key_name);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(FWSRV_SIGBLOCK_DIGEST, a.key_sha256, 32);
}

/* OTA-018, OTA-024: relative URLs land on the server; a critical offer and a withdrawn running
 * version are read; unknown keys are ignored */
static void test_check_answer_relative_and_unknown_keys(void)
{
    proto_fwsrv_answer_t a;
    const char *json =
        "{\"future\":{\"deep\":[1,{\"x\":null}]},\"update\":true,\"version\":\"v0.9.0-rc.1\","
        "\"critical\":true,\"installed\":{\"channel\":null,\"withdrawn\":true,\"why\":\"x\"},"
        "\"image\":{\"url\":\"/dl/a.bin\",\"extra\":[]}}";
    TEST_ASSERT_EQUAL(ALARM_OK, check(json, &a));
    TEST_ASSERT_EQUAL(PROTO_FWSRV_UPDATE, a.reason);
    TEST_ASSERT_EQUAL_STRING("https://firmware.example.net/dl/a.bin", a.image.url);
    TEST_ASSERT_EQUAL_STRING("", a.installed_channel); /* null: a local build */
    TEST_ASSERT_TRUE(a.installed_withdrawn);
    TEST_ASSERT_TRUE(a.critical);
    TEST_ASSERT_FALSE(a.image.has_sha256);
    TEST_ASSERT_FALSE(a.has_key);
    TEST_ASSERT_EQUAL_STRING("", a.key_name);

    json = "{\"update\":true,\"version\":\"0.9.0\",\"image\":{\"url\":\"a.bin\"}}";
    TEST_ASSERT_EQUAL(ALARM_OK, check(json, &a));
    TEST_ASSERT_EQUAL_STRING("https://firmware.example.net/a.bin", a.image.url);
    TEST_ASSERT_EQUAL(ALARM_OK, proto_fwsrv_check_answer(json, strlen(json),
                                                         "http://192.0.2.10:8080", &a));
    TEST_ASSERT_EQUAL_STRING("http://192.0.2.10:8080/a.bin", a.image.url);

    /* a channel this panel doesn't know is shown as none, not an error */
    json = "{\"update\":true,\"version\":\"0.9.0\",\"channel\":\"nightly\","
           "\"image\":{\"url\":\"a.bin\"}}";
    TEST_ASSERT_EQUAL(ALARM_OK, check(json, &a));
    TEST_ASSERT_EQUAL_STRING("", a.channel);

    /* a long key name is cut at a character, never mid-character */
    json = "{\"update\":true,\"version\":\"0.9.0\",\"image\":{\"url\":\"a.bin\",\"signing_key\":"
           "{\"name\":\"RoboAlarms release key for the stable channel \xc3\xa9\xc3\xa9\","
           "\"sha256\":\"" SHA_K "\"}}}";
    TEST_ASSERT_EQUAL(ALARM_OK, check(json, &a));
    TEST_ASSERT_TRUE(a.has_key);
    TEST_ASSERT_TRUE(strlen(a.key_name) < PROTO_FWSRV_KEY_NAME_MAX);
    TEST_ASSERT_EQUAL_STRING("RoboAlarms release key for the stable channel ", a.key_name);
}

/* OTA-018, OTA-024: every reason; withdrawn without an offer; the offer's fields mean nothing without an update */
static void test_check_answer_no_update(void)
{
    proto_fwsrv_answer_t a;
    TEST_ASSERT_EQUAL(ALARM_OK,
                      check("{\"installed\":{\"version\":\"0.8.0\",\"channel\":\"stable\","
                            "\"withdrawn\":false},\"update\":false,\"reason\":\"up_to_date\"}",
                            &a));
    TEST_ASSERT_EQUAL(PROTO_FWSRV_UP_TO_DATE, a.reason);
    TEST_ASSERT_EQUAL_STRING("stable", a.installed_channel);
    TEST_ASSERT_FALSE(a.installed_withdrawn);
    TEST_ASSERT_EQUAL(ALARM_OK,
                      check("{\"installed\":{\"version\":\"0.7.9\",\"channel\":\"beta\","
                            "\"withdrawn\":true},\"update\":false,\"reason\":\"held_back\"}",
                            &a));
    TEST_ASSERT_TRUE(a.installed_withdrawn);
    TEST_ASSERT_EQUAL_STRING("beta", a.installed_channel);

    static const struct {
        const char *reason;
        proto_fwsrv_reason_t want;
    } cases[] = {
        {"\"unknown_device\"", PROTO_FWSRV_UNKNOWN_DEVICE},
        {"\"needs_stepping_stone\"", PROTO_FWSRV_STEPPING_STONE},
        {"\"held_back\"", PROTO_FWSRV_HELD_BACK},
        {"\"moon_phase\"", PROTO_FWSRV_OTHER},
        {"\"a_reason_far_too_long_to_fit_in_the_buffer_at_all\"", PROTO_FWSRV_OTHER},
    };
    char json[256];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        snprintf(json, sizeof(json), "{\"update\":false,\"reason\":%s}", cases[i].reason);
        TEST_ASSERT_EQUAL_MESSAGE(ALARM_OK, check(json, &a), cases[i].reason);
        TEST_ASSERT_EQUAL_MESSAGE(cases[i].want, a.reason, cases[i].reason);
    }
    TEST_ASSERT_EQUAL(ALARM_OK, check("{\"update\":false}", &a));
    TEST_ASSERT_EQUAL(PROTO_FWSRV_UP_TO_DATE, a.reason);

    TEST_ASSERT_EQUAL(ALARM_OK,
                      check("{\"update\":false,\"reason\":\"held_back\",\"critical\":true,"
                            "\"version\":\"0.9.0\",\"image\":{\"url\":\"a.bin\"}}",
                            &a));
    TEST_ASSERT_EQUAL(PROTO_FWSRV_HELD_BACK, a.reason);
    TEST_ASSERT_FALSE(a.critical);
    TEST_ASSERT_EQUAL_STRING("", a.image.url);
    TEST_ASSERT_EQUAL_STRING("", a.image.version);
}

/* OTA-018: what isn't a check answer is refused, and the answer is left empty */
static void test_check_answer_refused(void)
{
    const char *bad[] = {
        "",
        "not json",
        "[]",
        "{}",
        "{\"update\":\"yes\"}",
        "{\"update\":true}",
        "{\"update\":true,\"version\":\"0.9.0\"}",
        "{\"update\":true,\"version\":\"0.9.0\",\"image\":{}}",
        "{\"update\":true,\"version\":\"0.9.0\",\"image\":{\"url\":\"\"}}",
        "{\"update\":true,\"version\":\"0.9.0\",\"image\":\"a.bin\"}",
        "{\"update\":true,\"image\":{\"url\":\"a.bin\"}}",
        "{\"update\":true,\"version\":\"834e0ad\",\"image\":{\"url\":\"a.bin\"}}",
        "{\"update\":true,\"version\":\"0.9.0\",\"image\":{\"url\":\"a.bin\",\"sha256\":\"00\"}}",
        "{\"update\":true,\"version\":\"0.9.0\",\"image\":{\"url\":\"a.bin\",\"sha256\":"
        "\"zz9a1c2b77d0e4150011223344556677889900aabbccddeeff00112233445566\"}}",
        "{\"update\":true,\"version\":\"0.9.0\",\"image\":{\"url\":\"a.bin\",\"size\":-1}}",
        "{\"update\":true,\"version\":\"0.9.0\",\"image\":{\"url\":\"a.bin\","
        "\"size\":4294967296}}",
        "{\"update\":true,\"version\":\"0.9.0\",\"image\":{\"url\":\"a.bin\",\"signing_key\":"
        "{\"name\":\"k\",\"sha256\":\"12\"}}}",
        "{\"update\":true,\"version\":\"0.9.0\",\"image\":{\"url\":\"a.bin\",\"signing_key\":"
        "{\"name\":\"k\"}}}",
        "{\"update\":false,\"installed\":\"x\"}",
        "{\"update\":false,\"installed\":{\"channel\":7}}",
        "{\"update\":false,\"critical\":\"no\"}",
        "{\"update\":false,\"reason\":7}",
        "{\"update\":false,}",
        "{\"update\":false} x",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        proto_fwsrv_answer_t a;
        memset(&a, 0x5A, sizeof(a));
        TEST_ASSERT_EQUAL_MESSAGE(ALARM_ERR_BAD_ARG, check(bad[i], &a), bad[i]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("", a.image.url, bad[i]);
        TEST_ASSERT_FALSE_MESSAGE(a.has_key, bad[i]);
    }
    /* a truncated real answer */
    proto_fwsrv_answer_t a;
    for (size_t cut = 0; cut < sizeof(UPDATE_ANSWER) - 1u; cut += 7u) {
        TEST_ASSERT_EQUAL(ALARM_ERR_BAD_ARG,
                          proto_fwsrv_check_answer(UPDATE_ANSWER, cut, BASE, &a));
    }
    TEST_ASSERT_EQUAL(ALARM_ERR_BAD_ARG, proto_fwsrv_check_answer(NULL, 0, BASE, &a));
}

/* OTA-018: a download or notes address is absolute http(s), printable, and https from an https
 * server; another host is allowed (a CDN). Null offer fields and a long key name don't refuse an
 * answer. */
static void test_check_answer_addresses(void)
{
    proto_fwsrv_answer_t a;
    const char *json = "{\"update\":true,\"version\":\"0.9.0\","
                       "\"image\":{\"url\":\"https://cdn.example.org/a.bin\"}}";
    TEST_ASSERT_EQUAL(ALARM_OK, check(json, &a));
    TEST_ASSERT_EQUAL_STRING("https://cdn.example.org/a.bin", a.image.url);
    TEST_ASSERT_EQUAL(ALARM_OK, proto_fwsrv_check_answer(json, strlen(json), NULL, &a));
    TEST_ASSERT_EQUAL_STRING("https://cdn.example.org/a.bin", a.image.url);

    const char *bad_urls[] = {
        "http://firmware.example.net/a.bin", /* from https down to http */
        "ftp://firmware.example.net/a.bin",
        "file:///a.bin",
        "a.bin?x=://",
        "https://",
        "https:///a.bin",
        "a b.bin",
        "a\\r\\n.bin",
        "a\\u007f.bin",
    };
    char buf[256];
    for (size_t i = 0; i < sizeof(bad_urls) / sizeof(bad_urls[0]); i++) {
        snprintf(buf, sizeof(buf),
                 "{\"update\":true,\"version\":\"0.9.0\",\"image\":{\"url\":\"%s\"}}",
                 bad_urls[i]);
        TEST_ASSERT_EQUAL_MESSAGE(ALARM_ERR_BAD_ARG, check(buf, &a), bad_urls[i]);
    }
    /* a relative address needs a server to be relative to */
    json = "{\"update\":true,\"version\":\"0.9.0\",\"image\":{\"url\":\"a.bin\"}}";
    TEST_ASSERT_EQUAL(ALARM_ERR_BAD_ARG, proto_fwsrv_check_answer(json, strlen(json), NULL, &a));

    /* the notes page: resolved like a download, or left out */
    json = "{\"update\":true,\"version\":\"0.9.0\",\"notes_url\":\"/releases/0.9.0\","
           "\"image\":{\"url\":\"a.bin\"}}";
    TEST_ASSERT_EQUAL(ALARM_OK, check(json, &a));
    TEST_ASSERT_EQUAL_STRING("https://firmware.example.net/releases/0.9.0", a.image.notes_url);
    json = "{\"update\":true,\"version\":\"0.9.0\",\"notes_url\":\"javascript:alert(1)\","
           "\"image\":{\"url\":\"a.bin\"}}";
    TEST_ASSERT_EQUAL(ALARM_OK, check(json, &a)); /* no scheme: a harmless page on the server */
    TEST_ASSERT_EQUAL_STRING("https://firmware.example.net/javascript:alert(1)",
                             a.image.notes_url);
    json = "{\"update\":true,\"version\":\"0.9.0\",\"notes_url\":\"javascript://%0aalert(1)\","
           "\"image\":{\"url\":\"a.bin\"}}";
    TEST_ASSERT_EQUAL(ALARM_OK, check(json, &a));
    TEST_ASSERT_EQUAL_STRING("", a.image.notes_url);
    TEST_ASSERT_EQUAL_STRING("https://firmware.example.net/a.bin", a.image.url);

    /* null offer fields: nothing without an update, refused with one */
    TEST_ASSERT_EQUAL(ALARM_OK, check("{\"update\":false,\"version\":null,\"critical\":null,"
                                      "\"image\":null,\"reason\":null}",
                                      &a));
    TEST_ASSERT_EQUAL(PROTO_FWSRV_UP_TO_DATE, a.reason);
    TEST_ASSERT_EQUAL(ALARM_ERR_BAD_ARG,
                      check("{\"update\":true,\"version\":\"0.9.0\",\"image\":null}", &a));
    TEST_ASSERT_EQUAL(ALARM_ERR_BAD_ARG,
                      check("{\"update\":true,\"version\":null,\"image\":{\"url\":\"a.bin\"}}",
                            &a));

    /* a key name too long to read: no name, the digest still counts (as proto_fwsrv_keys) */
    char name[200];
    memset(name, 'k', sizeof(name) - 1u);
    name[sizeof(name) - 1u] = '\0';
    char big[512];
    snprintf(big, sizeof(big),
             "{\"update\":true,\"version\":\"0.9.0\",\"image\":{\"url\":\"a.bin\","
             "\"signing_key\":{\"name\":\"%s\",\"sha256\":\"" SHA_K "\"}}}",
             name);
    TEST_ASSERT_EQUAL(ALARM_OK, check(big, &a));
    TEST_ASSERT_TRUE(a.has_key);
    TEST_ASSERT_EQUAL_STRING("", a.key_name);

    /* a language pack's address follows the same rule: a bad one is skipped */
    proto_fw_language_t langs[4];
    json = "{\"update\":true,\"version\":\"0.9.0\",\"image\":{\"url\":\"a.bin\"},\"languages\":["
           "{\"locale\":\"es-US\",\"url\":\"http://firmware.example.net/es.robolang\"},"
           "{\"locale\":\"fr-CA\",\"url\":\"lang/fr.robolang\"}]}";
    TEST_ASSERT_EQUAL(1u, proto_fwsrv_languages(json, strlen(json), BASE, langs, 4u));
    TEST_ASSERT_EQUAL_STRING("fr-CA", langs[0].locale);
    TEST_ASSERT_EQUAL_STRING("https://firmware.example.net/lang/fr.robolang", langs[0].url);
}

/* ---- language packs (OTA-025) ------------------------------------------------------------ */

/* OTA-025 */
static void test_languages(void)
{
    proto_fw_language_t l[4];
    size_t n = proto_fwsrv_languages(UPDATE_ANSWER, strlen(UPDATE_ANSWER), BASE, l, 4);
    TEST_ASSERT_EQUAL_size_t(1u, n);
    TEST_ASSERT_EQUAL_STRING("es-US", l[0].locale);
    TEST_ASSERT_EQUAL_STRING("0.8.0", l[0].version);
    TEST_ASSERT_EQUAL_STRING(
        "https://firmware.example.net/dl/0.8.0/es-US-0.8.0-6be04a1f.robolang", l[0].url);
    TEST_ASSERT_EQUAL_UINT32(155648u, l[0].size);
    TEST_ASSERT_TRUE(l[0].has_sha256);
    TEST_ASSERT_EQUAL_HEX8(0x6b, l[0].sha256[0]);

    const char *list =
        "{\"version\":\"0.8.0\",\"languages\":["
        "{\"locale\":\"es-US\",\"name\":\"Espa\\u00f1ol\",\"url\":\"/dl/es-US.robolang\","
        "\"size\":155648,\"sha256\":\"" SHA_B "\"},"
        "{\"locale\":\"fr-CA\",\"name\":\"Fran\\u00e7ais\",\"url\":\"fr-CA.robolang\","
        "\"version\":\"0.8.1\"},"
        "{\"locale\":\"x\",\"url\":\"bad-locale.robolang\"},"
        "{\"locale\":\"de-DE\"},"
        "{\"locale\":\"it-IT\",\"url\":\"it.robolang\",\"sha256\":\"nothex\"},"
        "{\"locale\":\"pt-BR\",\"url\":\"pt.robolang\",\"size\":-5}]}";
    n = proto_fwsrv_languages(list, strlen(list), BASE, l, 4);
    TEST_ASSERT_EQUAL_size_t(2u, n);
    TEST_ASSERT_EQUAL_STRING("https://firmware.example.net/dl/es-US.robolang", l[0].url);
    TEST_ASSERT_EQUAL_STRING("0.8.0", l[0].version);
    TEST_ASSERT_EQUAL_STRING("fr-CA", l[1].locale);
    TEST_ASSERT_EQUAL_STRING("https://firmware.example.net/fr-CA.robolang", l[1].url);
    TEST_ASSERT_EQUAL_STRING("0.8.1", l[1].version);
    TEST_ASSERT_FALSE(l[1].has_sha256);

    /* at most max */
    TEST_ASSERT_EQUAL_size_t(1u, proto_fwsrv_languages(list, strlen(list), BASE, l, 1));
    /* a check answer without packs */
    const char *none = "{\"update\":false,\"reason\":\"up_to_date\"}";
    TEST_ASSERT_EQUAL_size_t(0u, proto_fwsrv_languages(none, strlen(none), BASE, l, 4));

    /* not an answer of either kind */
    const char *bad[] = {"", "[]", "{}", "{\"version\":\"0.8.0\"}", "{\"languages\":{}}",
                         "{\"languages\":[1]}", "{\"update\":1,\"languages\":[]}",
                         "{\"version\":7,\"languages\":[]}", "{\"languages\":[]"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_size_t_MESSAGE(SIZE_MAX,
                                         proto_fwsrv_languages(bad[i], strlen(bad[i]), BASE, l, 4),
                                         bad[i]);
    }
    TEST_ASSERT_EQUAL_size_t(SIZE_MAX, proto_fwsrv_languages(NULL, 0, BASE, l, 4));
}

/* OPT-007, OTA-025: the asset packs in a check answer and in a GET /api/v1/languages answer */
static void test_assets(void)
{
    proto_fw_asset_t e[4];
    /* an answer of today's server, without the key: 0 entries */
    TEST_ASSERT_EQUAL_size_t(0u, proto_fwsrv_assets(UPDATE_ANSWER, strlen(UPDATE_ANSWER), BASE, e, 4));

    const char *check =
        "{\"update\":true,\"version\":\"0.9.0\",\"image\":{\"url\":\"a.bin\"},"
        "\"languages\":[{\"locale\":\"es-US\",\"url\":\"es.robolang\"}],"
        "\"assets\":[{\"kind\":\"letters\",\"url\":\"/dl/0.9.0/letters.roboasset\","
        "\"size\":98304,\"sha256\":\"" SHA_B "\"},"
        "{\"kind\":\"web\",\"url\":\"web.roboasset\"},"
        "{\"kind\":\"certificates\",\"url\":\"dl/certs.roboasset\",\"version\":\"0.9.1\"},"
        "{\"kind\":\"picture\"}]}";
    TEST_ASSERT_EQUAL_size_t(2u, proto_fwsrv_assets(check, strlen(check), BASE, e, 4));
    TEST_ASSERT_EQUAL(PROTO_FW_ASSET_LETTERS, e[0].kind);
    TEST_ASSERT_EQUAL_STRING("https://firmware.example.net/dl/0.9.0/letters.roboasset", e[0].url);
    TEST_ASSERT_EQUAL_STRING("0.9.0", e[0].version);
    TEST_ASSERT_EQUAL_UINT32(98304u, e[0].size);
    TEST_ASSERT_TRUE(e[0].has_sha256);
    TEST_ASSERT_EQUAL_HEX8(0x6b, e[0].sha256[0]);
    TEST_ASSERT_EQUAL(PROTO_FW_ASSET_CERTS, e[1].kind);
    TEST_ASSERT_EQUAL_STRING("https://firmware.example.net/dl/certs.roboasset", e[1].url);
    TEST_ASSERT_EQUAL_STRING("0.9.1", e[1].version);
    /* the languages still read beside it */
    proto_fw_language_t l[2];
    TEST_ASSERT_EQUAL_size_t(1u, proto_fwsrv_languages(check, strlen(check), BASE, l, 2));
    /* a value that is not a list (a server's empty list may come as null) reads as none and never
     * costs the languages */
    const char *nul = "{\"update\":true,\"version\":\"0.9.0\",\"image\":{\"url\":\"a.bin\"},"
                      "\"languages\":[{\"locale\":\"es-US\",\"url\":\"es.robolang\"}],\"assets\":null}";
    TEST_ASSERT_EQUAL_size_t(0u, proto_fwsrv_assets(nul, strlen(nul), BASE, e, 4));
    TEST_ASSERT_EQUAL_size_t(1u, proto_fwsrv_languages(nul, strlen(nul), BASE, l, 2));
    const char *obj = "{\"version\":\"0.8.0\",\"languages\":[],\"assets\":{\"kind\":\"picture\"}}";
    TEST_ASSERT_EQUAL_size_t(0u, proto_fwsrv_assets(obj, strlen(obj), BASE, e, 4));
    TEST_ASSERT_EQUAL_size_t(0u, proto_fwsrv_languages(obj, strlen(obj), BASE, l, 2));
    /* at most max */
    TEST_ASSERT_EQUAL_size_t(1u, proto_fwsrv_assets(check, strlen(check), BASE, e, 1));

    /* a GET /api/v1/languages answer with the running version's packs */
    const char *list = "{\"version\":\"0.8.0\",\"languages\":[],"
                       "\"assets\":[{\"kind\":\"picture\",\"url\":\"saver.roboasset\"}]}";
    TEST_ASSERT_EQUAL_size_t(1u, proto_fwsrv_assets(list, strlen(list), BASE, e, 4));
    TEST_ASSERT_EQUAL(PROTO_FW_ASSET_PICTURE, e[0].kind);
    TEST_ASSERT_EQUAL_STRING("0.8.0", e[0].version);
    TEST_ASSERT_EQUAL_STRING("https://firmware.example.net/saver.roboasset", e[0].url);

    /* not an answer of either kind */
    const char *bad[] = {"", "[]", "{}", "{\"version\":\"0.8.0\"}", "{\"assets\":{}}", "{\"assets\":null}",
                         "{\"assets\":[1]}", "{\"update\":1,\"assets\":[]}",
                         "{\"version\":7,\"assets\":[]}", "{\"assets\":[]"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_size_t_MESSAGE(SIZE_MAX,
                                         proto_fwsrv_assets(bad[i], strlen(bad[i]), BASE, e, 4),
                                         bad[i]);
    }
    TEST_ASSERT_EQUAL_size_t(SIZE_MAX, proto_fwsrv_assets(NULL, 0, BASE, e, 4));
    for (size_t cut = 0; cut < strlen(check); cut += 5u) {
        TEST_ASSERT_EQUAL_size_t(SIZE_MAX, proto_fwsrv_assets(check, cut, BASE, e, 4));
    }
}

/* OTA-025: the address of the running version's language packs, for a language added
 * later. */
static void test_languages_url(void)
{
    char out[256];
    proto_fwsrv_facts_t f = {.device = "crowpanel_dhe04107d", .chip_rev = "v3", .version = "0.8.0"};
    TEST_ASSERT_TRUE(proto_fwsrv_languages_url(BASE, &f, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(BASE "/api/v1/languages?device=crowpanel_dhe04107d&chip_rev=v3"
                                  "&version=0.8.0",
                             out);
    /* the signed project name adds the drafts; values are percent-encoded; a trailing "/" on the
     * base is not doubled; the check's other facts are not sent */
    f.project = "Alarm System+dev&x";
    f.channel = "beta";
    f.board_rev = "1.1";
    f.version = "0.9.0-rc.1";
    TEST_ASSERT_TRUE(proto_fwsrv_languages_url("http://192.0.2.10:8080/", &f, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("http://192.0.2.10:8080/api/v1/languages?device=crowpanel_dhe04107d"
                             "&chip_rev=v3&version=0.9.0-rc.1&project=Alarm%20System%2Bdev%26x",
                             out);
    /* a UTF-8 byte is encoded too */
    f.project = "caf\xc3\xa9";
    TEST_ASSERT_TRUE(proto_fwsrv_languages_url(BASE, &f, out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "&project=caf%C3%A9"));

    /* exactly fits; one byte less doesn't (never a cut-off address) */
    f.project = "";
    TEST_ASSERT_TRUE(proto_fwsrv_languages_url(BASE, &f, out, sizeof(out)));
    const size_t need = strlen(out) + 1u;
    char small[256];
    TEST_ASSERT_TRUE(proto_fwsrv_languages_url(BASE, &f, small, need));
    TEST_ASSERT_EQUAL_STRING(out, small);
    for (size_t cap = 1u; cap < need; cap++) {
        TEST_ASSERT_FALSE(proto_fwsrv_languages_url(BASE, &f, small, cap));
        TEST_ASSERT_EQUAL_STRING("", small);
    }
    f.project = "a b";
    TEST_ASSERT_FALSE(proto_fwsrv_languages_url(BASE, &f, small, need + 10u)); /* too short */

    /* a missing fact refuses */
    const proto_fwsrv_facts_t missing[] = {
        {.chip_rev = "v3", .version = "0.8.0"},
        {.device = "crowpanel_dhe04107d", .version = "0.8.0"},
        {.device = "crowpanel_dhe04107d", .chip_rev = "", .version = "0.8.0"},
        {.device = "crowpanel_dhe04107d", .chip_rev = "v3"},
    };
    for (size_t i = 0; i < sizeof(missing) / sizeof(missing[0]); i++) {
        TEST_ASSERT_FALSE(proto_fwsrv_languages_url(BASE, &missing[i], out, sizeof(out)));
        TEST_ASSERT_EQUAL_STRING("", out);
    }
    f.project = NULL;
    TEST_ASSERT_FALSE(proto_fwsrv_languages_url("", &f, out, sizeof(out)));
    TEST_ASSERT_FALSE(proto_fwsrv_languages_url(NULL, &f, out, sizeof(out)));
    TEST_ASSERT_FALSE(proto_fwsrv_languages_url(BASE, NULL, out, sizeof(out)));
    TEST_ASSERT_FALSE(proto_fwsrv_languages_url(BASE, &f, NULL, 64));
}

/* ---- signing keys (OTA-020) -------------------------------------------------------------- */

/* OTA-020 */
static void test_keys(void)
{
    const char *json =
        "{\"keys\":[{\"name\":\"RoboAlarms release\",\"sha256\":\"" SHA_K "\","
        "\"flasher_sha256\":\"" SHA_A "\",\"pem\":\"-----BEGIN PUBLIC KEY-----\\nMIIB\\n"
        "-----END PUBLIC KEY-----\\n\",\"future\":{\"a\":[1]}},"
        "{\"sha256\":\"" SHA_A "\"},"
        "{\"name\":\"no digest\"},"
        "{\"name\":\"bad digest\",\"sha256\":\"12\"},"
        "{\"name\":\"\",\"sha256\":\"" SHA_A "\"},"
        "{\"name\":\"RoboAlarms beta\",\"sha256\":\"" SHA_A "\"}],\"next\":null}";
    proto_fwsrv_key_t k[PROTO_FWSRV_KEYS_MAX];
    size_t n = proto_fwsrv_keys(json, strlen(json), k, PROTO_FWSRV_KEYS_MAX);
    TEST_ASSERT_EQUAL_size_t(2u, n);
    TEST_ASSERT_EQUAL_STRING("RoboAlarms release", k[0].name);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(FWSRV_SIGBLOCK_DIGEST, k[0].sha256, 32);
    TEST_ASSERT_EQUAL_STRING("RoboAlarms beta", k[1].name);
    TEST_ASSERT_EQUAL_HEX8(0x3f, k[1].sha256[0]);
    TEST_ASSERT_EQUAL_size_t(1u, proto_fwsrv_keys(json, strlen(json), k, 1));
    TEST_ASSERT_EQUAL_size_t(0u, proto_fwsrv_keys("{\"keys\":[]}", 11, k, 4));

    const char *bad[] = {"", "[]", "{}", "{\"keys\":{}}", "{\"keys\":[1]}", "{\"keys\":[}",
                         "{\"keys\":[]} trailing"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_size_t_MESSAGE(SIZE_MAX, proto_fwsrv_keys(bad[i], strlen(bad[i]), k, 4),
                                         bad[i]);
    }
    TEST_ASSERT_EQUAL_size_t(SIZE_MAX, proto_fwsrv_keys(NULL, 0, k, 4));
}

static proto_fwsrv_key_t key_of(const char *name, uint8_t first)
{
    proto_fwsrv_key_t k;
    memset(&k, 0, sizeof(k));
    snprintf(k.name, sizeof(k.name), "%s", name);
    memset(k.sha256, first, sizeof(k.sha256));
    return k;
}

/* OTA-020: the key a new server's check shows - the offered image's, else the only one, else one
 * the panel trusts, else the first; nothing when the server lists none. */
static void test_key_to_show(void)
{
    const proto_fwsrv_key_t keys[3] = {key_of("Old", 0x11), key_of("Release", 0x22),
                                       key_of("Beta", 0x33)};
    const uint8_t trusted[2][32] = {{0x99}, {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33,
                                             0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33,
                                             0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33,
                                             0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33}};
    proto_fwsrv_key_t out;

    /* the offered image's key wins, also over a trusted one; a missing name comes from the list */
    proto_fwsrv_key_t offered = key_of("", 0x22);
    TEST_ASSERT_TRUE(proto_fwsrv_key_to_show(&offered, true, keys, 3, trusted, 2, &out));
    TEST_ASSERT_EQUAL_STRING("Release", out.name);
    TEST_ASSERT_EQUAL_HEX8(0x22, out.sha256[31]);
    offered = key_of("Unlisted", 0x44); /* not in the list: shown as the image has it */
    TEST_ASSERT_TRUE(proto_fwsrv_key_to_show(&offered, true, keys, 3, trusted, 2, &out));
    TEST_ASSERT_EQUAL_STRING("Unlisted", out.name);
    TEST_ASSERT_EQUAL_HEX8(0x44, out.sha256[0]);
    offered = key_of("", 0x44);
    TEST_ASSERT_TRUE(proto_fwsrv_key_to_show(&offered, true, NULL, 0, NULL, 0, &out));
    TEST_ASSERT_EQUAL_STRING("", out.name);

    /* has_offered false: offered is ignored */
    TEST_ASSERT_TRUE(proto_fwsrv_key_to_show(&offered, false, &keys[1], 1, trusted, 2, &out));
    TEST_ASSERT_EQUAL_STRING("Release", out.name); /* the only key */
    TEST_ASSERT_TRUE(proto_fwsrv_key_to_show(NULL, false, keys, 3, trusted, 2, &out));
    TEST_ASSERT_EQUAL_STRING("Beta", out.name); /* the trusted one */
    TEST_ASSERT_TRUE(proto_fwsrv_key_to_show(NULL, false, keys, 3, trusted, 1, &out));
    TEST_ASSERT_EQUAL_STRING("Old", out.name); /* none trusted: the first */
    TEST_ASSERT_TRUE(proto_fwsrv_key_to_show(NULL, false, keys, 3, NULL, 2, &out));
    TEST_ASSERT_EQUAL_STRING("Old", out.name);

    /* no key anywhere */
    out = keys[0];
    TEST_ASSERT_FALSE(proto_fwsrv_key_to_show(NULL, false, keys, 0, trusted, 2, &out));
    TEST_ASSERT_EQUAL_STRING("", out.name);
    TEST_ASSERT_EQUAL_HEX8(0x00, out.sha256[0]);
    TEST_ASSERT_FALSE(proto_fwsrv_key_to_show(NULL, false, NULL, 3, NULL, 0, &out));
}

/* OTA-020: alarm_crc32 is the CRC ESP-IDF's esp_rom_crc32_le(0, ...) and espsecure's
 * zlib.crc32 compute (CRC-32/ISO-HDLC, check value 0xCBF43926). */
static void test_crc_matches_esp_idf(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, alarm_crc32(0u, "123456789", 9));
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, alarm_crc32(0u, "", 0));
}

/* OTA-020: the digest of the key in a real block espsecure made equals its own
 * digest-sbv2-public-key; damage of any kind is refused. */
static void test_sigblock_key(void)
{
    uint8_t d[32];
    TEST_ASSERT_TRUE(proto_fwsrv_sigblock_key(FWSRV_SIGBLOCK, sizeof(FWSRV_SIGBLOCK), d));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(FWSRV_SIGBLOCK_DIGEST, d, 32);

    uint8_t b[1300];
    memset(b, 0xFF, sizeof(b));
    memcpy(b, FWSRV_SIGBLOCK, sizeof(FWSRV_SIGBLOCK));
    /* more after the block (the rest of the signature sector) is ignored */
    TEST_ASSERT_TRUE(proto_fwsrv_sigblock_key(b, sizeof(b), d));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(FWSRV_SIGBLOCK_DIGEST, d, 32);

    TEST_ASSERT_FALSE(proto_fwsrv_sigblock_key(b, 1215, d)); /* short */
    TEST_ASSERT_FALSE(proto_fwsrv_sigblock_key(b, 0, d));
    TEST_ASSERT_FALSE(proto_fwsrv_sigblock_key(NULL, 1216, d));

    b[0] = 0xE6; /* wrong magic */
    TEST_ASSERT_FALSE(proto_fwsrv_sigblock_key(b, 1216, d));
    b[0] = FWSRV_SIGBLOCK[0];
    b[1] = 0x03; /* wrong version (an ECDSA block is 3) */
    TEST_ASSERT_FALSE(proto_fwsrv_sigblock_key(b, 1216, d));
    b[1] = FWSRV_SIGBLOCK[1];
    b[1196] ^= 0x01; /* the CRC itself */
    TEST_ASSERT_FALSE(proto_fwsrv_sigblock_key(b, 1216, d));
    b[1196] = FWSRV_SIGBLOCK[1196];
    b[500] ^= 0x80; /* a bit of the key */
    TEST_ASSERT_FALSE(proto_fwsrv_sigblock_key(b, 1216, d));
    b[500] = FWSRV_SIGBLOCK[500];
    b[1100] ^= 0x01; /* a bit of the signature */
    TEST_ASSERT_FALSE(proto_fwsrv_sigblock_key(b, 1216, d));
    b[1100] = FWSRV_SIGBLOCK[1100];
    b[1210] ^= 0xFF; /* the padding isn't covered */
    TEST_ASSERT_TRUE(proto_fwsrv_sigblock_key(b, 1216, d));
}

/* OTA-020 */
static void test_fingerprint(void)
{
    uint8_t d[32] = {0x3f, 0x9a, 0x1c, 0x2b, 0x77, 0xd0, 0xe4, 0x15, 0xff, 0xee};
    char out[PROTO_FWSRV_FINGERPRINT_LEN];
    memset(out, 'x', sizeof(out));
    proto_fwsrv_fingerprint(d, out);
    TEST_ASSERT_EQUAL_STRING("3F9A 1C2B 77D0 E415", out);
    TEST_ASSERT_EQUAL_size_t(PROTO_FWSRV_FINGERPRINT_LEN - 1u, strlen(out));
    proto_fwsrv_fingerprint(FWSRV_SIGBLOCK_DIGEST, out);
    TEST_ASSERT_EQUAL_STRING("4D84 4CC7 24EE 6789", out);
}

/* ---- the trust record (OTA-020) ---------------------------------------------------------- */

static proto_fwsrv_trust_t sample_trust(void)
{
    proto_fwsrv_trust_t t;
    memset(&t, 0, sizeof(t));
    snprintf(t.server, sizeof(t.server), "builds.example.org");
    snprintf(t.key_name, sizeof(t.key_name), "Harbor builds");
    memcpy(t.key_sha256, FWSRV_SIGBLOCK_DIGEST, 32);
    t.has_key = true;
    return t;
}

/* OTA-020 */
static void test_trust_round_trip(void)
{
    const proto_fwsrv_trust_t t = sample_trust();
    uint8_t blob[PROTO_FWSRV_TRUST_BLOB_MAX];
    const size_t n = proto_fwsrv_trust_encode(&t, blob, sizeof(blob));
    TEST_ASSERT_TRUE(n > 0u && n <= PROTO_FWSRV_TRUST_BLOB_MAX);
    proto_fwsrv_trust_t back;
    TEST_ASSERT_TRUE(proto_fwsrv_trust_decode(blob, n, &back));
    TEST_ASSERT_EQUAL_MEMORY(&t, &back, sizeof(t));

    /* without a key: decodes back the same */
    proto_fwsrv_trust_t nk;
    memset(&nk, 0, sizeof(nk));
    snprintf(nk.server, sizeof(nk.server), "example.net");
    const size_t m = proto_fwsrv_trust_encode(&nk, blob, sizeof(blob));
    TEST_ASSERT_TRUE(m > 0u && m < n);
    TEST_ASSERT_TRUE(proto_fwsrv_trust_decode(blob, m, &back));
    TEST_ASSERT_EQUAL_MEMORY(&nk, &back, sizeof(nk));
    TEST_ASSERT_FALSE(back.has_key);

    /* a development address is stored as it is */
    snprintf(nk.server, sizeof(nk.server), "http://192.0.2.10:8080");
    const size_t d = proto_fwsrv_trust_encode(&nk, blob, sizeof(blob));
    TEST_ASSERT_TRUE(proto_fwsrv_trust_decode(blob, d, &back));
    TEST_ASSERT_EQUAL_STRING("http://192.0.2.10:8080", back.server);

    /* the largest record fits PROTO_FWSRV_TRUST_BLOB_MAX */
    proto_fwsrv_trust_t big = sample_trust();
    memset(big.server, 'a', sizeof(big.server) - 1u);
    big.server[sizeof(big.server) - 1u] = '\0';
    memset(big.key_name, 'k', sizeof(big.key_name) - 1u);
    big.key_name[sizeof(big.key_name) - 1u] = '\0';
    const size_t b = proto_fwsrv_trust_encode(&big, blob, sizeof(blob));
    TEST_ASSERT_TRUE(b > 0u && b <= PROTO_FWSRV_TRUST_BLOB_MAX);
    TEST_ASSERT_TRUE(proto_fwsrv_trust_decode(blob, b, &back));
    TEST_ASSERT_EQUAL_MEMORY(&big, &back, sizeof(big));
}

/* OTA-020: version 1's layout, frozen: a reader must keep reading what this one wrote */
static void test_trust_layout_v1(void)
{
    proto_fwsrv_trust_t t;
    memset(&t, 0, sizeof(t));
    snprintf(t.server, sizeof(t.server), "example.net");
    snprintf(t.key_name, sizeof(t.key_name), "K");
    for (unsigned i = 0; i < 32u; i++) {
        t.key_sha256[i] = (uint8_t)i;
    }
    t.has_key = true;
    uint8_t blob[PROTO_FWSRV_TRUST_BLOB_MAX];
    const size_t n = proto_fwsrv_trust_encode(&t, blob, sizeof(blob));
    static const uint8_t head[] = {0x54, 0x01, 0x01, 11, 'e', 'x', 'a', 'm', 'p', 'l',
                                   'e', '.', 'n', 'e', 't', 1, 'K', 0x00, 0x01};
    TEST_ASSERT_EQUAL_size_t(4u + 11u + 2u + 32u + 4u, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(head, blob, sizeof(head));
    const uint32_t crc = alarm_crc32(0u, blob, n - 4u);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)crc, blob[n - 4u]);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(crc >> 24), blob[n - 1u]);
}

/* OTA-020: damage, short input and unknown versions are refused (the panel then uses its own
 * build's server) */
static void test_trust_refused(void)
{
    const proto_fwsrv_trust_t t = sample_trust();
    uint8_t blob[PROTO_FWSRV_TRUST_BLOB_MAX + 8u];
    const size_t n = proto_fwsrv_trust_encode(&t, blob, sizeof(blob));
    proto_fwsrv_trust_t back;

    for (size_t len = 0; len < n; len++) { /* every shorter length */
        TEST_ASSERT_FALSE(proto_fwsrv_trust_decode(blob, len, &back));
    }
    for (size_t i = 0; i < n; i++) { /* every flipped bit of each byte's low bit */
        blob[i] ^= 0x01;
        TEST_ASSERT_FALSE(proto_fwsrv_trust_decode(blob, n, &back));
        blob[i] ^= 0x01;
    }
    TEST_ASSERT_TRUE(proto_fwsrv_trust_decode(blob, n, &back));
    blob[n] = 0;
    TEST_ASSERT_FALSE(proto_fwsrv_trust_decode(blob, n + 1u, &back)); /* trailing byte */
    TEST_ASSERT_FALSE(proto_fwsrv_trust_decode(NULL, n, &back));

    /* a well-formed record of an unknown version or with unknown flags, its CRC right */
    static const uint8_t versions[][3] = {{0x54, 0x02, 0x01}, {0x54, 0x00, 0x01},
                                          {0x55, 0x01, 0x01}, {0x54, 0x01, 0x03}};
    for (size_t v = 0; v < sizeof(versions) / sizeof(versions[0]); v++) {
        uint8_t copy[PROTO_FWSRV_TRUST_BLOB_MAX];
        memcpy(copy, blob, n);
        memcpy(copy, versions[v], 3);
        const uint32_t crc = alarm_crc32(0u, copy, n - 4u);
        copy[n - 4u] = (uint8_t)crc;
        copy[n - 3u] = (uint8_t)(crc >> 8);
        copy[n - 2u] = (uint8_t)(crc >> 16);
        copy[n - 1u] = (uint8_t)(crc >> 24);
        TEST_ASSERT_FALSE(proto_fwsrv_trust_decode(copy, n, &back));
        TEST_ASSERT_EQUAL_STRING("", back.server);
    }

    /* lengths that point past the record, or a NUL inside a string, with a right CRC */
    static const uint8_t bad_len[] = {0x54, 0x01, 0x00, 40, 'a', 'b', 0, 0, 0, 0};
    static const uint8_t no_server[] = {0x54, 0x01, 0x00, 0, 0, 0, 0, 0, 0};
    static const uint8_t nul[] = {0x54, 0x01, 0x00, 2, 'a', 0, 0, 0, 0, 0, 0};
    static const uint8_t no_key[] = {0x54, 0x01, 0x01, 1, 'a', 0, 0, 0, 0, 0};
    const struct {
        const uint8_t *p;
        size_t n;
    } forged[] = {{bad_len, sizeof(bad_len)}, {no_server, sizeof(no_server)},
                  {nul, sizeof(nul)}, {no_key, sizeof(no_key)}};
    for (size_t f = 0; f < sizeof(forged) / sizeof(forged[0]); f++) {
        uint8_t copy[16];
        memcpy(copy, forged[f].p, forged[f].n);
        const size_t body = forged[f].n - 4u;
        const uint32_t crc = alarm_crc32(0u, copy, body);
        copy[body] = (uint8_t)crc;
        copy[body + 1u] = (uint8_t)(crc >> 8);
        copy[body + 2u] = (uint8_t)(crc >> 16);
        copy[body + 3u] = (uint8_t)(crc >> 24);
        TEST_ASSERT_FALSE_MESSAGE(proto_fwsrv_trust_decode(copy, forged[f].n, &back),
                                  "forged record");
    }

    /* encode refuses what it can't store */
    proto_fwsrv_trust_t e;
    memset(&e, 0, sizeof(e));
    TEST_ASSERT_EQUAL_size_t(0u, proto_fwsrv_trust_encode(&e, blob, sizeof(blob))); /* no server */
    e = sample_trust();
    memset(e.server, 'a', sizeof(e.server)); /* no NUL */
    TEST_ASSERT_EQUAL_size_t(0u, proto_fwsrv_trust_encode(&e, blob, sizeof(blob)));
    e = sample_trust();
    TEST_ASSERT_EQUAL_size_t(0u, proto_fwsrv_trust_encode(&e, blob, n - 1u)); /* too small */
    TEST_ASSERT_EQUAL_size_t(n, proto_fwsrv_trust_encode(&e, blob, n));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_domain_accepts_and_normalises);
    RUN_TEST(test_domain_refuses);
    RUN_TEST(test_base_url);
    RUN_TEST(test_channels);
    RUN_TEST(test_check_request);
    RUN_TEST(test_check_answer_update);
    RUN_TEST(test_check_answer_relative_and_unknown_keys);
    RUN_TEST(test_check_answer_no_update);
    RUN_TEST(test_check_answer_refused);
    RUN_TEST(test_check_answer_addresses);
    RUN_TEST(test_languages);
    RUN_TEST(test_assets);
    RUN_TEST(test_languages_url);
    RUN_TEST(test_keys);
    RUN_TEST(test_key_to_show);
    RUN_TEST(test_crc_matches_esp_idf);
    RUN_TEST(test_sigblock_key);
    RUN_TEST(test_fingerprint);
    RUN_TEST(test_trust_round_trip);
    RUN_TEST(test_trust_layout_v1);
    RUN_TEST(test_trust_refused);
    return UNITY_END();
}
