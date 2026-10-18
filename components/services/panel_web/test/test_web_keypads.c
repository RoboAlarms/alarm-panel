/* The production HTTP service, with deterministic alarm/link services. Both UI build modes. */
#include "ui_backend/ui_backend.h"
#include "unity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CONFIG_ALARM_LINK    1
#define ESP_OK               0
#define PLINK_SERVICE_MASTER 0
typedef unsigned nvs_handle_t;
#define NVS_READONLY  0
#define NVS_READWRITE 1
static char saved_name[ALARM_NAME_LEN];
static int nvs_open(const char *ns, int mode, nvs_handle_t *h)
{
    (void)ns;
    (void)mode;
    *h = 1;
    return ESP_OK;
}
#if CONFIG_ALARM_FEATURE_UI
static int nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    (void)h;
    (void)key;
    (void)len;
    strcpy(out, saved_name);
    return ESP_OK;
}
#endif
static int nvs_set_str(nvs_handle_t h, const char *key, const char *value)
{
    (void)h;
    (void)key;
    strcpy(saved_name, value);
    return ESP_OK;
}
static int nvs_commit(nvs_handle_t h)
{
    (void)h;
    return ESP_OK;
}
static void nvs_close(nvs_handle_t h)
{
    (void)h;
}
typedef struct {
    uint8_t partition_home;
    bool switch_allowed, return_never;
    uint16_t return_s;
    int volume, brightness;
} panel_prefs_t;
static panel_prefs_t fake_prefs;
static alarm_config_t config;
static ui_link_pair_t pair;
static ui_link_keypad_t keypad;
static bool programming, other_owner, armed;
static int64_t now_us;
static unsigned opens, decisions, exits;
static void *alarm_mem_slow_calloc(size_t n, size_t size)
{
    return calloc(n, size);
}
static int64_t esp_timer_get_time(void)
{
    return now_us;
}
static void alarm_service_get_snapshot(alarm_snapshot_t *s)
{
    memset(s, 0, sizeof *s);
    s->program = programming;
}
static alarm_result_t alarm_service_program_enter_as(const alarm_source_t *s, uint8_t user)
{
    (void)s;
    if (user != 1 || armed || programming)
        return ALARM_ERR_NOT_AUTHORIZED;
    programming = true;
    return ALARM_OK;
}
static void alarm_service_program_exit_owned(const alarm_source_t *s)
{
    (void)s;
    if (!other_owner)
        programming = false;
    exits++;
}
static int alarm_service_get_config(alarm_config_t *out)
{
    *out = config;
    return ESP_OK;
}
static void panel_prefs_load(panel_prefs_t *out)
{
    *out = fake_prefs;
}
static int panel_prefs_save(const panel_prefs_t *p)
{
    fake_prefs = *p;
    return ESP_OK;
}
static int plink_service_role(void)
{
    return PLINK_SERVICE_MASTER;
}
static bool plink_service_pair_status(void *c, ui_link_pair_t *out)
{
    (void)c;
    *out = pair;
    return true;
}
static size_t plink_service_managed_keypads(void *c, ui_link_keypad_t *out, size_t max)
{
    (void)c;
    if (keypad.device && max) {
        *out = keypad;
        return 1;
    }
    return 0;
}
static uint8_t plink_service_manage(void *c, const ui_link_req_t *r)
{
    (void)c;
    if (!programming && r->kind != UI_LINK_REQ_PAIR_STOP)
        return UI_LINK_REQ_NOT_ALLOWED;
    switch (r->kind) {
    case UI_LINK_REQ_PAIR_OPEN:
        opens++;
        pair.state = UI_LINK_PAIR_OPEN;
        break;
    case UI_LINK_REQ_PAIR_DECIDE:
        if (strcmp(r->pair_code, pair.code))
            return UI_LINK_REQ_REFUSED;
        decisions++;
        pair.state = r->allow ? UI_LINK_PAIR_WAITING : UI_LINK_PAIR_FAILED;
        break;
    case UI_LINK_REQ_PAIR_STOP:
        pair.state = UI_LINK_PAIR_FAILED;
        pair.fail = UI_LINK_FAIL_CANCELLED;
        break;
    case UI_LINK_REQ_KEYPAD_SET:
        keypad.home = r->home;
        snprintf(keypad.name, sizeof keypad.name, "%s", r->name);
        break;
    case UI_LINK_REQ_KEYPAD_REMOVE: keypad.device = 0; break;
    case UI_LINK_REQ_PARTITION_POLICY:
        keypad.switch_allowed = r->switch_allowed;
        keypad.return_never = r->return_never;
        keypad.return_s = r->return_s;
        break;
    default: return UI_LINK_REQ_REFUSED;
    }
    return UI_LINK_REQ_OK;
}
#include "../src/web_keypads.c"
void setUp(void)
{
    memset(&attempt, 0, sizeof attempt);
    memset(&pair, 0, sizeof pair);
    memset(&config, 0, sizeof config);
    memset(&keypad, 0, sizeof keypad);
    config.partitions[0].enabled = config.partitions[1].enabled = true;
    fake_prefs =
        (panel_prefs_t){.partition_home = 1, .return_s = 30, .volume = 0, .brightness = 80};
    programming = other_owner = armed = false;
    now_us = 0;
    opens = decisions = exits = 0;
}
void tearDown(void)
{
}
static const char *post_as(uint8_t user, const char *text)
{
    cJSON *o = cJSON_Parse(text);
    TEST_ASSERT_NOT_NULL(o);
    const char *why = web_keypads_post(user, o);
    cJSON_Delete(o);
    return why;
}
#define START "{\"op\":\"start\",\"attempt\":\"0123456789abcdef0123456789abcdef\"}"
#define CONFIRM                                                                                    \
    "{\"op\":\"confirm\",\"attempt\":\"0123456789abcdef0123456789abcdef\",\"code\":\"588460\","    \
    "\"sequence\":0,\"allow\":true}"
static void enrollment_is_owned_and_idempotent(void)
{
    TEST_ASSERT_NOT_NULL(post_as(2, START));
    TEST_ASSERT_NULL(post_as(1, START));
    TEST_ASSERT_NULL(post_as(1, START));
    TEST_ASSERT_EQUAL_UINT(1, opens);
    pair.state = UI_LINK_PAIR_COMPARE;
    strcpy(pair.code, "588460");
    TEST_ASSERT_NOT_NULL(post_as(2, CONFIRM));
    TEST_ASSERT_NOT_NULL(post_as(1, "{\"op\":\"cancel\",\"attempt\":\"old\"}"));
    strcpy(pair.code, "123456");
    TEST_ASSERT_NOT_NULL(post_as(1, CONFIRM));
    TEST_ASSERT_EQUAL_UINT(0, decisions);
    strcpy(pair.code, "588460");
    TEST_ASSERT_NULL(post_as(1, CONFIRM));
    TEST_ASSERT_NULL(post_as(1, CONFIRM));
    TEST_ASSERT_EQUAL_UINT(1, decisions);
    TEST_ASSERT_TRUE(programming);
    pair.state = UI_LINK_PAIR_DONE;
    pair.device = 2;
    web_keypads_tick(NULL);
    TEST_ASSERT_FALSE(programming);
    TEST_ASSERT_EQUAL_UINT(2, attempt.pair.device);
    TEST_ASSERT_NULL(post_as(1, START));
    TEST_ASSERT_EQUAL_UINT(1, opens); /* lost start response */
}
static void expires_without_browser_and_keeps_other_owner(void)
{
    TEST_ASSERT_NULL(post_as(1, START));
    now_us = 120000001;
    other_owner = true;
    web_keypads_tick(NULL);
    TEST_ASSERT_FALSE(attempt.active);
    TEST_ASSERT_TRUE(programming);
    TEST_ASSERT_EQUAL_UINT(UI_LINK_FAIL_TIMEOUT, attempt.pair.fail);
}
static void headless_and_local_policy(void)
{
    cJSON *o = web_keypads_get(1);
    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_EQUAL_INT(CONFIG_ALARM_FEATURE_UI,
                          cJSON_GetObjectItemCaseSensitive(o, "local") != NULL);
    cJSON_Delete(o);
    const char *why =
        post_as(1, "{\"op\":\"save\",\"device\":0,\"name\":\"Main "
                   "keypad\",\"home\":2,\"allowed\":true,\"never\":true,\"seconds\":45}");
#if CONFIG_ALARM_FEATURE_UI
    TEST_ASSERT_NULL(why);
    TEST_ASSERT_EQUAL_UINT(2, fake_prefs.partition_home);
    TEST_ASSERT_TRUE(fake_prefs.return_never);
#else
    TEST_ASSERT_NOT_NULL(why);
#endif
    TEST_ASSERT_EQUAL_INT(0, fake_prefs.volume);
    TEST_ASSERT_EQUAL_INT(80, fake_prefs.brightness);
    TEST_ASSERT_NOT_NULL(post_as(1, "{\"op\":\"remove\",\"device\":0}"));
}
static void protects_assigned_partitions_and_paired_slots(void)
{
    keypad.device = 2;
    keypad.home = 2;
    cJSON *c = cJSON_Parse("{\"devices\":[{\"number\":2,\"enabled\":false}]}");
    TEST_ASSERT_FALSE(web_keypads_config_valid(c, &config));
    cJSON_Delete(c);
    c = cJSON_CreateObject();
    config.partitions[1].enabled = false;
    TEST_ASSERT_FALSE(web_keypads_config_valid(c, &config));
    keypad.device = 0;
    config.devices[3].enabled = true;
    config.devices[3].partition = 2;
    TEST_ASSERT_FALSE(web_keypads_config_valid(c, &config));
    cJSON_Delete(c);
    armed = true;
    TEST_ASSERT_NOT_NULL(post_as(1, START));
    TEST_ASSERT_EQUAL_UINT(0, opens);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(enrollment_is_owned_and_idempotent);
    RUN_TEST(expires_without_browser_and_keeps_other_owner);
    RUN_TEST(headless_and_local_policy);
    RUN_TEST(protects_assigned_partitions_and_paired_slots);
    return UNITY_END();
}
