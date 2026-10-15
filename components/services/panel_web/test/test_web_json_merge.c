/* web_json_merge on the PC (docs/features/31 WEB-016, config/display; code review
 * finding 6): GET config/display merges the voice switches into the display's values (an
 * object) and fields (an array). Every allocation the merge makes is failed in turn: it must
 * report the failure, free what it took, and never crash. */
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "web_json_merge.h"

static long s_live;      /* allocations not yet freed */
static long s_allowed;   /* allocations left before the next one fails; <0: no limit */
static long s_attempts;  /* allocations asked for since the limit was set */

static void *counting_malloc(size_t n)
{
    s_attempts++;
    if (s_allowed == 0)
        return NULL;
    if (s_allowed > 0)
        s_allowed--;
    void *p = malloc(n);
    if (p)
        s_live++;
    return p;
}
static void counting_free(void *p)
{
    if (p)
        s_live--;
    free(p);
}

void setUp(void)
{
    cJSON_Hooks hooks = {counting_malloc, counting_free};
    cJSON_InitHooks(&hooks);
    s_allowed = -1;
    s_live = 0;
}
void tearDown(void)
{
    cJSON_InitHooks(NULL);
}

/* The display's document as domain_get builds it: values and fields, each with one member. */
static cJSON *display_doc(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *values = cJSON_AddObjectToObject(o, "values");
    cJSON_AddNumberToObject(values, "brightness", 80);
    cJSON *fields = cJSON_AddArrayToObject(o, "fields");
    cJSON *f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "name", "brightness");
    cJSON_AddItemToArray(fields, f);
    return o;
}
/* The voice switches as fields_get builds them (three members), and their schema (three items). */
static const char *const k_voice[] = {"voice_enabled", "voice_status", "voice_chime"};
static cJSON *voice_values(void)
{
    cJSON *o = cJSON_CreateObject();
    for (unsigned i = 0; i < 3; i++)
        cJSON_AddBoolToObject(o, k_voice[i], i % 2 == 0);
    return o;
}
static cJSON *voice_schema(void)
{
    cJSON *a = cJSON_CreateArray();
    for (unsigned i = 0; i < 3; i++) {
        cJSON *f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "name", k_voice[i]);
        cJSON_AddItemToArray(a, f);
    }
    return a;
}

/* WEB-016: the merge as it runs with memory to spare. */
static void test_merge_moves_every_member(void)
{
    cJSON *o = display_doc();
    cJSON *values = cJSON_GetObjectItemCaseSensitive(o, "values");
    cJSON *fields = cJSON_GetObjectItemCaseSensitive(o, "fields");
    TEST_ASSERT_TRUE(web_json_move_members(values, voice_values()));
    TEST_ASSERT_TRUE(web_json_move_elements(fields, voice_schema()));
    TEST_ASSERT_EQUAL_INT(4, cJSON_GetArraySize(values));
    TEST_ASSERT_EQUAL_INT(4, cJSON_GetArraySize(fields));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(values, "voice_enabled")));
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(values, "voice_status")));
    cJSON *last = cJSON_GetArrayItem(fields, 3);
    TEST_ASSERT_EQUAL_STRING("voice_chime", cJSON_GetObjectItemCaseSensitive(last, "name")->valuestring);
    cJSON_Delete(o);
    TEST_ASSERT_EQUAL_INT(0, s_live);
}

/* Review 3 #6: no document, a missing values/fields, or a temporary that couldn't be made is
 * reported, never dereferenced; whatever was handed over is freed. */
static void test_merge_refuses_missing_parts(void)
{
    TEST_ASSERT_FALSE(web_json_move_members(NULL, voice_values()));
    TEST_ASSERT_FALSE(web_json_move_elements(NULL, voice_schema()));
    TEST_ASSERT_FALSE(web_json_move_members(NULL, NULL));
    cJSON *o = display_doc();
    TEST_ASSERT_FALSE(web_json_move_members(cJSON_GetObjectItemCaseSensitive(o, "values"), NULL));
    TEST_ASSERT_FALSE(web_json_move_elements(cJSON_GetObjectItemCaseSensitive(o, "fields"), NULL));
    /* The wrong kind on either side: an array into an object, an object onto an array. */
    TEST_ASSERT_FALSE(web_json_move_members(cJSON_GetObjectItemCaseSensitive(o, "values"), voice_schema()));
    TEST_ASSERT_FALSE(web_json_move_elements(cJSON_GetObjectItemCaseSensitive(o, "fields"), voice_values()));
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(o, "values")));
    cJSON_Delete(o);
    TEST_ASSERT_EQUAL_INT(0, s_live);
}

/* Review 3 #6: the object loop copies each name into `into`; fail each copy in turn. */
static void test_member_loop_survives_each_failed_allocation(void)
{
    long failing_at = 0;
    for (;; failing_at++) {
        cJSON *o = display_doc();
        cJSON *more = voice_values();
        s_attempts = 0;
        s_allowed = failing_at;
        const bool ok = web_json_move_members(cJSON_GetObjectItemCaseSensitive(o, "values"), more);
        const long asked = s_attempts;
        s_allowed = -1;
        cJSON_Delete(o);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_live, "leaked after a failed allocation");
        if (ok) {
            TEST_ASSERT_TRUE(asked <= failing_at); /* it succeeded only with what it was allowed */
            break;
        }
        TEST_ASSERT_TRUE(asked > failing_at); /* a refusal only when an allocation failed */
    }
    TEST_ASSERT_EQUAL_INT(3, failing_at); /* one name per voice switch: three were failed in turn */
}

/* Review 3 #6: the array loop allocates nothing, so it always completes; the schema temporary
 * that would have been short a field is caught by the caller's count (web_config.c). */
static void test_element_loop_needs_no_memory(void)
{
    cJSON *o = display_doc();
    cJSON *more = voice_schema();
    s_attempts = 0;
    s_allowed = 0;
    TEST_ASSERT_TRUE(web_json_move_elements(cJSON_GetObjectItemCaseSensitive(o, "fields"), more));
    s_allowed = -1;
    TEST_ASSERT_EQUAL_INT(4, cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(o, "fields")));
    cJSON_Delete(o);
    TEST_ASSERT_EQUAL_INT(0, s_live);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_merge_moves_every_member);
    RUN_TEST(test_merge_refuses_missing_parts);
    RUN_TEST(test_member_loop_survives_each_failed_allocation);
    RUN_TEST(test_element_loop_needs_no_memory);
    return UNITY_END();
}
