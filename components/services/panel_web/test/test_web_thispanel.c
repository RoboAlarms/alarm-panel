/* Setup through the server link: a backup in parts, the sirens and outputs, Smart Home's codes. */
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "web_thispanel.h"

void setUp(void) {}
void tearDown(void) {}

static void test_base64_round_trips(void)
{
    const uint8_t in[] = {0, 1, 2, 250, 251, 252, 253};
    char text[16];
    uint8_t out[8];
    for (size_t n = 1; n <= sizeof(in); n++) {
        web_b64_encode(in, n, text);
        TEST_ASSERT_EQUAL_INT((int)n, web_b64_decode(text, out, sizeof(out)));
        TEST_ASSERT_EQUAL_MEMORY(in, out, n);
    }
    TEST_ASSERT_EQUAL_size_t(0, web_b64_encode(in, 0, text));
    web_b64_encode((const uint8_t *)"RABK", 4, text);
    TEST_ASSERT_EQUAL_STRING("UkFCSw==", text);
    TEST_ASSERT_EQUAL_INT(-1, web_b64_decode("UkFCSw=", out, sizeof(out)));   /* not a multiple of 4 */
    TEST_ASSERT_EQUAL_INT(-1, web_b64_decode("Uk=CSw==", out, sizeof(out)));  /* padding inside */
    TEST_ASSERT_EQUAL_INT(-1, web_b64_decode("UkFC!w==", out, sizeof(out)));  /* not base64 */
    TEST_ASSERT_EQUAL_INT(-1, web_b64_decode("UkFCSw==", out, 3));           /* doesn't fit */
}

/* A 70 000-byte backup goes out in three parts of at most WEB_PART_OUT, and comes back whole. */
static void test_a_backup_goes_out_and_back_in_parts(void)
{
    const size_t len = 70000;
    uint8_t *file = malloc(len), *back = malloc(len);
    for (size_t i = 0; i < len; i++)
        file[i] = (uint8_t)(i * 7u + 3u);
    size_t got = 0, parts = 0;
    while (got < len) {
        cJSON *p = web_part_json(file, len, got);
        TEST_ASSERT_NOT_NULL(p);
        TEST_ASSERT_EQUAL_DOUBLE((double)got, cJSON_GetNumberValue(cJSON_GetObjectItem(p, "offset")));
        const char *data = cJSON_GetStringValue(cJSON_GetObjectItem(p, "data"));
        TEST_ASSERT_TRUE(strlen(data) <= 40000u); /* under the link's 48 KB answer */
        const int n = web_b64_decode(data, back + got, len - got);
        TEST_ASSERT_TRUE(n > 0 && (size_t)n <= WEB_PART_OUT);
        got += (size_t)n;
        parts++;
        cJSON_Delete(p);
    }
    TEST_ASSERT_EQUAL(3, parts);
    TEST_ASSERT_EQUAL_MEMORY(file, back, len);
    TEST_ASSERT_NULL(web_part_json(file, len, len));

    /* A restore takes parts of WEB_PART_IN in order, and nothing past the size declared. */
    memset(back, 0, len);
    size_t have = 0;
    char *text = malloc(4u * ((WEB_PART_IN + 2u) / 3u) + 1u);
    while (have < len) {
        const size_t n = len - have < WEB_PART_IN ? len - have : WEB_PART_IN;
        web_b64_encode(file + have, n, text);
        TEST_ASSERT_NULL(web_part_take(back, len, &have, have, text));
    }
    TEST_ASSERT_EQUAL_MEMORY(file, back, len);
    web_b64_encode(file, 8, text);
    TEST_ASSERT_EQUAL_STRING("backup_format", web_part_take(back, len, &have, have, text)); /* past the size */
    have = 10;
    TEST_ASSERT_EQUAL_STRING("backup_format", web_part_take(back, len, &have, 0, text));    /* out of order */
    TEST_ASSERT_EQUAL_STRING("backup_format", web_part_take(back, len, &have, 10, "**"));    /* not base64 */
    TEST_ASSERT_EQUAL(10, have);
    free(text);
    free(file);
    free(back);
}

static void test_sirens_list_their_settings_and_the_free_terminals(void)
{
    web_wired_t w[OW_CHANNELS];
    web_rf_t r[RFS_SIRENS];
    memset(w, 0, sizeof(w));
    memset(r, 0, sizeof(r));
    w[1].c = (ow_config_t){.function = OW_FN_SIREN, .gpio = 21, .partitions = 1, .burglary = true, .fire = true, .sense = OW_SENSE_NONE};
    w[1].trouble = true;
    r[0].c = (rfs_config_t){.used = true, .partitions = 3, .burglary = true};
    const uint8_t terms[] = {20, 22};
    cJSON *o = web_sirens_json(w, OW_CHANNELS, r, RFS_SIRENS, terms, 2);
    char *s = cJSON_PrintUnformatted(o);
    TEST_ASSERT_EQUAL_STRING("{\"outputs\":[{\"number\":2,\"kind\":\"siren\",\"name\":\"\",\"on\":false,\"trouble\":true,"
                             "\"partitions\":1,\"also\":0,\"burglary\":true,\"fire\":true,\"squawk\":false,\"terminal\":21,"
                             "\"active_low\":false,\"sense\":null},{\"number\":1,\"kind\":\"rf_siren\",\"name\":\"\",\"on\":false,"
                             "\"trouble\":false,\"partitions\":3,\"also\":0,\"burglary\":true,\"fire\":false,\"squawk\":false}],"
                             "\"terminals\":[20,22]}",
                             s);
    free(s);
    cJSON_Delete(o);
}

static void test_a_siren_request_is_read_strictly(void)
{
    bool rf = true;
    size_t slot = 9;
    ow_config_t c = {.function = OW_FN_SIREN, .gpio = 21, .partitions = 1, .burglary = true, .sense = OW_SENSE_NONE};
    rfs_config_t rc = {.used = true, .partitions = 1};
    cJSON *o = cJSON_Parse("{\"action\":\"set\",\"kind\":\"siren\",\"number\":2,\"partitions\":3,\"squawk\":true,\"sense\":22}");
    TEST_ASSERT_EQUAL(WEB_SIREN_SET, web_siren_request(o, &rf, &slot, &c, &rc));
    TEST_ASSERT_FALSE(rf);
    TEST_ASSERT_EQUAL(1, slot);
    TEST_ASSERT_EQUAL(3, c.partitions);
    TEST_ASSERT_TRUE(c.squawk && c.burglary);
    TEST_ASSERT_EQUAL(22, c.sense);
    TEST_ASSERT_EQUAL(21, c.gpio); /* not named: kept */
    cJSON_Delete(o);
    o = cJSON_Parse("{\"action\":\"test\",\"kind\":\"rf_siren\",\"number\":4}");
    TEST_ASSERT_EQUAL(WEB_SIREN_TEST, web_siren_request(o, &rf, &slot, &c, &rc));
    TEST_ASSERT_TRUE(rf);
    TEST_ASSERT_EQUAL(3, slot);
    cJSON_Delete(o);
    static const char *const BAD[] = {"{\"action\":\"test\",\"kind\":\"siren\",\"number\":5}",
                                      "{\"action\":\"test\",\"kind\":\"rf_siren\",\"number\":0}",
                                      "{\"action\":\"test\",\"kind\":\"bell\",\"number\":1}",
                                      "{\"action\":\"melt\",\"kind\":\"siren\",\"number\":1}",
                                      "{\"action\":\"set\",\"kind\":\"siren\",\"number\":1,\"name\":\"a name far too long for a channel\"}",
                                      "{\"kind\":\"siren\",\"number\":1}"};
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        o = cJSON_Parse(BAD[i]);
        TEST_ASSERT_EQUAL_MESSAGE(WEB_SIREN_BAD, web_siren_request(o, &rf, &slot, &c, &rc), BAD[i]);
        cJSON_Delete(o);
    }
}

/* BL-220: which devices ask for an Owner code, and whether arranging does, from the server. */
static void test_smart_home_codes_change_from_the_server(void)
{
    hc_home_t *h = calloc(1, sizeof(*h));
    hc_home_clear(h);
    const web_home_dev_t devs[] = {{"lock.front", "Front door", "Hall"}, {"light.porch", "Porch light", ""}};
    cJSON *o = cJSON_Parse("{\"arrange_code\":true,\"devices\":[{\"ref\":\"lock.front\",\"code\":true}]}");
    TEST_ASSERT_TRUE(web_home_apply(h, o, devs, 2));
    cJSON_Delete(o);
    TEST_ASSERT_TRUE(hc_home_protected(h, "lock.front"));
    TEST_ASSERT_FALSE(hc_home_protected(h, "light.porch"));
    TEST_ASSERT_TRUE(hc_home_arrange_code(h));
    cJSON *j = web_home_json(h, devs, 2);
    char *s = cJSON_PrintUnformatted(j);
    TEST_ASSERT_EQUAL_STRING("{\"devices\":[{\"ref\":\"lock.front\",\"name\":\"Front door\",\"room\":\"Hall\",\"code\":true},"
                             "{\"ref\":\"light.porch\",\"name\":\"Porch light\",\"room\":\"\",\"code\":false}],\"arrange_code\":true}",
                             s);
    free(s);
    cJSON_Delete(j);
    /* A device the panel doesn't have, or nothing to take: refused, nothing changed. */
    o = cJSON_Parse("{\"arrange_code\":false,\"devices\":[{\"ref\":\"lock.back\",\"code\":true}]}");
    TEST_ASSERT_FALSE(web_home_apply(h, o, devs, 2));
    cJSON_Delete(o);
    TEST_ASSERT_TRUE(hc_home_arrange_code(h));
    o = cJSON_Parse("{\"rooms\":[]}");
    TEST_ASSERT_FALSE(web_home_apply(h, o, devs, 2));
    cJSON_Delete(o);
    o = cJSON_Parse("{\"devices\":[{\"ref\":\"lock.front\",\"code\":false}]}");
    TEST_ASSERT_TRUE(web_home_apply(h, o, devs, 2));
    cJSON_Delete(o);
    TEST_ASSERT_FALSE(hc_home_protected(h, "lock.front"));
    free(h);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_base64_round_trips);
    RUN_TEST(test_a_backup_goes_out_and_back_in_parts);
    RUN_TEST(test_sirens_list_their_settings_and_the_free_terminals);
    RUN_TEST(test_a_siren_request_is_read_strictly);
    RUN_TEST(test_smart_home_codes_change_from_the_server);
    return UNITY_END();
}
