/* The app's two read-only answers: the sirens and outputs, and the last backup. */
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "web_readouts.h"

void setUp(void) {}
void tearDown(void) {}

static void expect_json(const char *want, cJSON *o)
{
    TEST_ASSERT_NOT_NULL(o);
    char *s = cJSON_PrintUnformatted(o);
    TEST_ASSERT_EQUAL_STRING(want, s);
    free(s);
    cJSON_Delete(o);
}

static void test_output_kinds(void)
{
    TEST_ASSERT_NULL(web_output_kind(0));
    TEST_ASSERT_EQUAL_STRING("siren", web_output_kind(1));
    TEST_ASSERT_EQUAL_STRING("automation", web_output_kind(5));
    TEST_ASSERT_NULL(web_output_kind(6));
}

static void test_outputs_list_the_used_ones(void)
{
    const web_output_t outs[] = {
        {.used = true, .number = 1, .kind = "siren", .name = "", .on = false},
        {.used = false, .number = 2, .kind = NULL},
        {.used = true, .number = 3, .kind = "automation", .name = "Porch light", .on = true},
        {.used = true, .number = 1, .kind = "rf_siren", .name = NULL, .on = true, .trouble = true},
    };
    expect_json("{\"outputs\":[{\"number\":1,\"kind\":\"siren\",\"name\":\"\",\"on\":false,\"trouble\":false},"
                "{\"number\":3,\"kind\":\"automation\",\"name\":\"Porch light\",\"on\":true,\"trouble\":false},"
                "{\"number\":1,\"kind\":\"rf_siren\",\"name\":\"\",\"on\":true,\"trouble\":true}]}",
                web_outputs_json(outs, 4));
    expect_json("{\"outputs\":[]}", web_outputs_json(outs, 0));
}

static void test_backup_state(void)
{
    expect_json("{\"last_at\":\"2026-11-03T07:00:00Z\",\"by\":2,\"by_name\":\"Alex\",\"size\":18432}",
                web_backup_json(true, 1793689200u, 2, "Alex", 18432u));
    expect_json("{\"last_at\":null,\"by\":2,\"by_name\":null,\"size\":900}",
                web_backup_json(true, 0u, 2, "", 900u)); /* the clock wasn't set */
    expect_json("{\"last_at\":null,\"by\":null,\"by_name\":null,\"size\":null}",
                web_backup_json(false, 0u, 0, NULL, 0u));
}

/* The app's local discovery: what the panel announces, and srv only once it is linked. */
static void test_app_discovery_txt(void)
{
    web_app_txt_t t;
    web_app_txt(&t, "30eda0123456c0de", "Home", "", 0);
    TEST_ASSERT_EQUAL_UINT(2, t.n); /* never a firmware version or a model */
    TEST_ASSERT_EQUAL_STRING("id", t.key[0]);
    TEST_ASSERT_EQUAL_STRING("30eda0123456c0de", t.value[0]);
    TEST_ASSERT_EQUAL_STRING("name", t.key[1]);
    TEST_ASSERT_EQUAL_STRING("Home", t.value[1]);
    web_app_txt(&t, "30eda0123456c0de", "Home", "cloud.roboalarms.com", 443);
    TEST_ASSERT_EQUAL_UINT(3, t.n);
    TEST_ASSERT_EQUAL_STRING("srv", t.key[2]);
    TEST_ASSERT_EQUAL_STRING("https://cloud.roboalarms.com", t.value[2]);
    web_app_txt(&t, "30eda0123456c0de", "Home", "192.0.2.5", 8443);
    TEST_ASSERT_EQUAL_STRING("https://192.0.2.5:8443", t.value[2]);
}

/* WEB-032: the panic types the app may offer, named in its order, and a request's type. */
static void test_app_panic(void)
{
    expect_json("[]", web_app_panic_json(0u));
    expect_json("[\"police\",\"fire\",\"medical\",\"silent\"]", web_app_panic_json(0x0Fu));
    expect_json("[\"medical\"]", web_app_panic_json((uint8_t)(1u << ALARM_PANIC_MEDICAL)));
    expect_json("[\"fire\"]", web_app_panic_json((uint8_t)(0xF0u | (1u << ALARM_PANIC_FIRE))));
    alarm_panic_t p = ALARM_PANIC_POLICE;
    TEST_ASSERT_TRUE(web_app_panic_type("silent", &p));
    TEST_ASSERT_EQUAL(ALARM_PANIC_SILENT, p);
    TEST_ASSERT_TRUE(web_app_panic_type("fire", &p));
    TEST_ASSERT_EQUAL(ALARM_PANIC_FIRE, p);
    TEST_ASSERT_FALSE(web_app_panic_type("Police", &p));
    TEST_ASSERT_FALSE(web_app_panic_type("", &p));
    TEST_ASSERT_FALSE(web_app_panic_type(NULL, &p));
    TEST_ASSERT_TRUE(web_app_panic_allowed(0x02u, ALARM_PANIC_SILENT));
    TEST_ASSERT_FALSE(web_app_panic_allowed(0x02u, ALARM_PANIC_POLICE));
    TEST_ASSERT_FALSE(web_app_panic_allowed(0x0Fu, (alarm_panic_t)9));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_output_kinds);
    RUN_TEST(test_outputs_list_the_used_ones);
    RUN_TEST(test_backup_state);
    RUN_TEST(test_app_discovery_txt);
    RUN_TEST(test_app_panic);
    return UNITY_END();
}
