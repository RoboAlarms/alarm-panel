#include "storage.h"
#include "unity.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}
static storage_sd_t ready(void)
{
    storage_sd_t sd = {.state = STORAGE_READY, .generation = 17,
                      .capacity_bytes = UINT64_C(64000000000)};
    return sd;
}
/* SD-003: deny unauthorized, armed, and active file consumers independently. */
static void test_protection(void)
{
    storage_sd_t sd = ready();
    TEST_ASSERT_TRUE(storage_allowed(&sd, STORAGE_FORMAT, 17, true, true, false));
    TEST_ASSERT_FALSE(storage_allowed(&sd, STORAGE_FORMAT, 17, false, true, false));
    TEST_ASSERT_FALSE(storage_allowed(&sd, STORAGE_FORMAT, 17, true, false, false));
    TEST_ASSERT_FALSE(storage_allowed(&sd, STORAGE_FORMAT, 17, true, true, true));
}
/* SD-004: a confirmation from before probe/removal cannot erase a replacement. */
static void test_stale(void)
{
    storage_sd_t sd = ready();
    TEST_ASSERT_FALSE(storage_allowed(&sd, STORAGE_FORMAT, 16, true, true, false));
    TEST_ASSERT_FALSE(storage_allowed(&sd, STORAGE_EJECT, 16, true, true, false));
    TEST_ASSERT_FALSE(storage_allowed(NULL, STORAGE_FORMAT, 17, true, true, false));
}
/* SD-001/003: unsupported media is only format-able after positive identification. */
static void test_media_states(void)
{
    storage_sd_t sd = ready();
    sd.state = STORAGE_UNSUPPORTED;
    TEST_ASSERT_TRUE(storage_allowed(&sd, STORAGE_FORMAT, 17, true, true, false));
    sd.capacity_bytes = 0;
    TEST_ASSERT_FALSE(storage_allowed(&sd, STORAGE_FORMAT, 17, true, true, false));
    sd = ready();
    for (int state = STORAGE_STARTING; state <= STORAGE_WORKING; state++) {
        sd.state = (storage_state_t)state;
        const bool allowed = state == STORAGE_READY || state == STORAGE_UNSUPPORTED;
        TEST_ASSERT_EQUAL(allowed, storage_allowed(&sd, STORAGE_FORMAT, 17, true, true, false));
    }
}
/* SD-006: eject stays latched until a deliberate recheck, and working cannot be reentered. */
static void test_eject_and_busy(void)
{
    storage_sd_t sd = ready();
    TEST_ASSERT_TRUE(storage_allowed(&sd, STORAGE_EJECT, 17, true, true, false));
    sd.state = STORAGE_EJECTED;
    TEST_ASSERT_TRUE(storage_allowed(&sd, STORAGE_CHECK, 17, true, true, false));
    TEST_ASSERT_FALSE(storage_allowed(&sd, STORAGE_EJECT, 17, true, true, false));
    sd.state = STORAGE_WORKING;
    TEST_ASSERT_FALSE(storage_allowed(&sd, STORAGE_CHECK, 17, true, true, false));
    TEST_ASSERT_FALSE(storage_allowed(&sd, (storage_action_t)99, 17, true, true, false));
}
/* SD-002: >4 GiB cards and full media must not wrap into enormous available space. */
static void test_capacity(void)
{
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(48000000000), storage_free(UINT64_C(64000000000), UINT64_C(16000000000)));
    TEST_ASSERT_EQUAL_UINT64(0, storage_free(4096, 4096));
    TEST_ASSERT_EQUAL_UINT64(0, storage_free(0, 4096));
}
static void test_units(void)
{
    char out[48];
    storage_size(out, sizeof(out), UINT64_C(64000000000));
    TEST_ASSERT_EQUAL_STRING("59.6 GiB", out);
    storage_size(out, sizeof(out), 1024u * 1024u);
    TEST_ASSERT_EQUAL_STRING("1.0 MiB", out);
    storage_size(out, sizeof(out), 0);
    TEST_ASSERT_EQUAL_STRING("0 B", out);
    storage_size(out, sizeof(out), UINT64_MAX);
    TEST_ASSERT_NOT_NULL(strstr(out, "TiB"));
}
/* STOR-001: signature overhead and alignment consume real slot space, unlike free RAM. */
static void test_signed_image(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, storage_image_bytes(0, 16384, true));
    TEST_ASSERT_EQUAL_UINT32(12288, storage_image_bytes(4097, 16384, true));
    TEST_ASSERT_EQUAL_UINT32(8192, storage_image_bytes(4096, 16384, true));
    TEST_ASSERT_EQUAL_UINT32(4097, storage_image_bytes(4097, 16384, false));
    TEST_ASSERT_EQUAL_UINT32(0, storage_image_bytes(4097, 8192, true));
    TEST_ASSERT_EQUAL_UINT32(0, storage_image_bytes(UINT32_MAX, UINT32_MAX, true));
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_protection); RUN_TEST(test_stale); RUN_TEST(test_media_states);
    RUN_TEST(test_eject_and_busy); RUN_TEST(test_capacity); RUN_TEST(test_units);
    RUN_TEST(test_signed_image);
    return UNITY_END();
}
