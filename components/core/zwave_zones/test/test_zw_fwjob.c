/* ZW-033 (docs/features/36): a device's software update up to the transfer - the check's request
 * and every kind of answer, what the device page shows, and the downloaded file checked against
 * the promised SHA-256 before it becomes the image (raw, Intel HEX, refused formats). The service's
 * answers are shaped like its v4 answer; device and URLs are fictional. */
#include <stdio.h>
#include <string.h>

#include "alarm_proto/proto_link.h"
#include "unity.h"
#include "zwave_zones/zw_fwjob.h"

void setUp(void) {}
void tearDown(void) {}

static zwfj_t J;
static zw_node_record_t REC;
static char BODY[ZWFJ_BODY_MAX];
static char ANSWER[2048];

static void record(void)
{
    memset(&REC, 0, sizeof REC);
    REC.node_id = 7u;
    REC.generation = 3u;
    REC.has_identity = true;
    REC.manufacturer_id = 0x0123u;
    REC.product_type = 0x0456u;
    REC.product_id = 0x0789u;
    REC.has_versions = true;
    REC.fw_major = 1u;
    REC.fw_minor = 18u;
}

static void sha_text(const uint8_t *data, size_t len, char out[72])
{
    uint8_t d[32];
    proto_sha256(data, len, d);
    int k = snprintf(out, 72, "sha256:");
    for (size_t i = 0; i < 32u; i++) {
        k += snprintf(&out[k], (size_t)(72 - k), "%02x", d[i]);
    }
}

/* An answer offering version 1.20 as `file_name` with `data`'s digest (and a second target when
 * `two`). */
static const char *offer(const char *file_name, const uint8_t *data, size_t len, bool two,
                         const char *changelog)
{
    char sha[72];
    sha_text(data, len, sha);
    snprintf(ANSWER, sizeof ANSWER,
             "[{\"manufacturerId\":\"0x0123\",\"productType\":\"0x0456\",\"productId\":\"0x0789\","
             "\"firmwareVersion\":\"1.18\",\"updates\":[{\"version\":\"1.20\",\"changelog\":\"%s\","
             "\"channel\":\"stable\",\"downgrade\":false,\"files\":[{\"target\":0,\"url\":"
             "\"https://example.com/fw/%s\",\"integrity\":\"%s\"}%s]}]}]",
             changelog, file_name, sha,
             two ? ",{\"target\":1,\"url\":\"https://example.com/fw/t1.gbl\",\"integrity\":"
                   "\"sha256:0000000000000000000000000000000000000000000000000000000000000000\"}"
                 : "");
    return ANSWER;
}

static void check(int status, const char *answer)
{
    record();
    TEST_ASSERT_TRUE(zwfj_check_begin(&J, &REC, "usa", BODY, sizeof BODY) > 0u);
    zwfj_check_answer(&J, status, answer, answer != NULL ? strlen(answer) : 0u, "usa");
}

static ui_zw_device_page_t page_of(uint16_t node, uint16_t generation)
{
    ui_zw_device_page_t p;
    memset(&p, 0, sizeof p);
    zwfj_page(&J, node, generation, &p);
    return p;
}

/* ZW-033: the check asks about exactly this device and version; a device whose identity or
 * versions aren't known yet isn't asked about. */
static void test_check_request(void)
{
    zwfj_init(&J);
    record();
    TEST_ASSERT_TRUE(zwfj_check_begin(&J, &REC, "usa", BODY, sizeof BODY) > 0u);
    TEST_ASSERT_NOT_NULL(strstr(BODY, "\"productId\":\"0x0789\""));
    TEST_ASSERT_NOT_NULL(strstr(BODY, "\"firmwareVersion\":\"1.18\""));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FW_CHECKING, page_of(7u, 3u).fw.check);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FW_NONE, page_of(7u, 4u).fw.check); /* another device */
    REC.has_versions = false;
    TEST_ASSERT_EQUAL_UINT(0u, zwfj_check_begin(&J, &REC, "usa", BODY, sizeof BODY));
    TEST_ASSERT_EQUAL_UINT(0u, zwfj_check_begin(&J, NULL, "usa", BODY, sizeof BODY));
}

/* ZW-033: each answer in words the installer can act on; a late answer to an older question is
 * not taken. */
static void test_answers(void)
{
    check(0, NULL);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FW_CHECK_FAILED, J.state);
    TEST_ASSERT_NOT_NULL(strstr(J.words, "internet connection"));
    check(503, "");
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FW_CHECK_FAILED, J.state);
    TEST_ASSERT_NOT_NULL(strstr(J.words, "(503)"));
    check(200, "not json");
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FW_CHECK_FAILED, J.state);
    check(200, "[]");
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FW_UNKNOWN, J.state);
    check(200, "[{\"manufacturerId\":\"0x0123\",\"productType\":\"0x0456\",\"productId\":"
               "\"0x0789\",\"updates\":[]}]");
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FW_UP_TO_DATE, J.state);

    static const uint8_t GBL[] = {0xEB, 0x17, 0xA6, 0x03, 1, 2, 3, 4};
    check(200, offer("dev.gbl", GBL, sizeof GBL, false, "Fixes the\\nbattery report.  Faster."));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FW_AVAILABLE, J.state);
    const ui_zw_device_page_t p = page_of(7u, 3u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FW_AVAILABLE, p.fw.check);
    TEST_ASSERT_EQUAL_STRING("1.20", p.fw.version);
    TEST_ASSERT_EQUAL_STRING("Fixes the battery report. Faster.", p.fw.words); /* one line */

    /* the same answer again: not asked, not taken */
    zwfj_check_answer(&J, 0, NULL, 0u, "usa");
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FW_AVAILABLE, J.state);

    /* more than the device's own firmware: named, never installed */
    check(200, offer("dev.gbl", GBL, sizeof GBL, true, ""));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FW_CHECK_FAILED, J.state);
    TEST_ASSERT_NOT_NULL(strstr(J.words, "1.20"));
    TEST_ASSERT_NULL(zwfj_download_begin(&J, 7u, 3u));

    /* long notes: cut with "..." */
    char log[400];
    memset(log, 'a', 150u);
    log[150] = '\0';
    check(200, offer("dev.gbl", GBL, sizeof GBL, false, log));
    TEST_ASSERT_EQUAL_UINT(sizeof J.words - 1u, strlen(J.words));
    TEST_ASSERT_EQUAL_STRING("...", &J.words[sizeof J.words - 4u]);
}

/* ZW-033: the file is checked against the digest before anything else; a raw image goes as it
 * is, Intel HEX is converted, other kinds are refused; the page says why. */
static void test_file(void)
{
    static uint8_t out[64];
    static const uint8_t GBL[] = {0xEB, 0x17, 0xA6, 0x03, 1, 2, 3, 4};
    check(200, offer("dev.gbl", GBL, sizeof GBL, false, ""));
    TEST_ASSERT_NULL(zwfj_download_begin(&J, 7u, 4u)); /* not the device offered for */
    TEST_ASSERT_NOT_NULL(zwfj_download_begin(&J, 7u, 3u));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FW_DOWNLOADING, page_of(7u, 3u).fw.check);
    uint8_t changed[sizeof GBL];
    memcpy(changed, GBL, sizeof GBL);
    changed[7] ^= 1u;
    TEST_ASSERT_EQUAL_UINT(0u, zwfj_file(&J, changed, sizeof changed, out, sizeof out));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FW_FILE_FAILED, J.state);
    TEST_ASSERT_NOT_NULL(strstr(J.words, "promised"));

    check(200, offer("dev.gbl", GBL, sizeof GBL, false, ""));
    TEST_ASSERT_NOT_NULL(zwfj_download_begin(&J, 7u, 3u));
    TEST_ASSERT_EQUAL_UINT(sizeof GBL, zwfj_file(&J, GBL, sizeof GBL, out, sizeof out));
    TEST_ASSERT_EQUAL_MEMORY(GBL, out, sizeof GBL);
    zwfj_started(&J);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FW_NONE, page_of(7u, 3u).fw.check);

    /* too big for the panel's buffer */
    check(200, offer("dev.gbl", GBL, sizeof GBL, false, ""));
    TEST_ASSERT_NOT_NULL(zwfj_download_begin(&J, 7u, 3u));
    TEST_ASSERT_EQUAL_UINT(0u, zwfj_file(&J, GBL, sizeof GBL, out, 4u));
    TEST_ASSERT_NOT_NULL(strstr(J.words, "too big"));

    /* Intel HEX: two bytes at 0 */
    static const char HEX[] = ":02000000ABCD86\n:00000001FF\n";
    check(200, offer("dev.hex", (const uint8_t *)HEX, strlen(HEX), false, ""));
    TEST_ASSERT_NOT_NULL(zwfj_download_begin(&J, 7u, 3u));
    TEST_ASSERT_EQUAL_UINT(2u, zwfj_file(&J, (const uint8_t *)HEX, strlen(HEX), out, sizeof out));
    TEST_ASSERT_EQUAL_HEX8(0xABu, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0xCDu, out[1]);

    /* a vendor container: refused */
    static const uint8_t OTZ[] = {0x5A, 0x01, 0x02};
    check(200, offer("dev.otz", OTZ, sizeof OTZ, false, ""));
    TEST_ASSERT_NOT_NULL(zwfj_download_begin(&J, 7u, 3u));
    TEST_ASSERT_EQUAL_UINT(0u, zwfj_file(&J, OTZ, sizeof OTZ, out, sizeof out));
    TEST_ASSERT_NOT_NULL(strstr(J.words, "can't read"));

    /* stopped while downloading: back to the offer, with its notes; its file then not taken */
    check(200, offer("dev.gbl", GBL, sizeof GBL, false, "Fixes."));
    TEST_ASSERT_NOT_NULL(zwfj_download_begin(&J, 7u, 3u));
    TEST_ASSERT_TRUE(zwfj_cancel(&J));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FW_AVAILABLE, J.state);
    TEST_ASSERT_EQUAL_STRING("Fixes.", J.words);
    TEST_ASSERT_EQUAL_UINT(0u, zwfj_file(&J, GBL, sizeof GBL, out, sizeof out));
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FW_AVAILABLE, J.state);
    TEST_ASSERT_FALSE(zwfj_cancel(&J));

    /* the download itself failed */
    check(200, offer("dev.gbl", GBL, sizeof GBL, false, ""));
    TEST_ASSERT_NOT_NULL(zwfj_download_begin(&J, 7u, 3u));
    zwfj_download_failed(&J, 404);
    TEST_ASSERT_EQUAL_UINT8(UI_ZW_FW_FILE_FAILED, J.state);
    TEST_ASSERT_NOT_NULL(strstr(J.words, "(404)"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_check_request);
    RUN_TEST(test_answers);
    RUN_TEST(test_file);
    return UNITY_END();
}
