/*
 * test_voice_epoch.c - spoken-name settling by source-change epoch (zone_voice_epoch.h;
 * docs/features/08 voice-first-build.md D07, CORE-CFG-021). The calls follow zone_service's
 * order: note the change, capture what is owed, invalidate (the alarm task), mark it, capture
 * the bindings, store them, mark them.
 */
#include <string.h>

#include "unity.h"
#include "zone_hub/zone_voice_epoch.h"

static zone_voice_epochs_t V;

void setUp(void)
{
    zone_voice_init(&V);
}

void tearDown(void) {}

static bool bit(const uint8_t *m, uint16_t zone)
{
    return (m[(zone - 1u) / 8u] & (1u << ((zone - 1u) % 8u))) != 0u;
}

/* D07: nothing owed or settling at start; a change makes its zone, and only it, settle until
 * its invalidation and then its binding are stored. */
static void test_one_change_settles_after_both_stores(void)
{
    uint8_t owed[ALARM_ZONE_BITMAP_BYTES];
    TEST_ASSERT_FALSE(zone_voice_any_owed(&V));
    TEST_ASSERT_FALSE(zone_voice_owed(&V, owed));
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        TEST_ASSERT_FALSE(zone_voice_settling(&V, z));
    }
    TEST_ASSERT_TRUE(zone_voice_note_change(&V, 5));
    TEST_ASSERT_TRUE(zone_voice_settling(&V, 5));
    TEST_ASSERT_FALSE(zone_voice_settling(&V, 4));
    TEST_ASSERT_TRUE(zone_voice_any_owed(&V));
    TEST_ASSERT_TRUE(zone_voice_owed(&V, owed));
    TEST_ASSERT_TRUE(bit(owed, 5));
    TEST_ASSERT_FALSE(bit(owed, 4));
    zone_voice_invalidated(&V, owed);
    TEST_ASSERT_FALSE(zone_voice_any_owed(&V));
    /* invalidated, binding not stored yet: an approval attempted now is refused */
    TEST_ASSERT_TRUE(zone_voice_settling(&V, 5));
    zone_voice_capture(&V);
    zone_voice_stored(&V);
    TEST_ASSERT_FALSE(zone_voice_settling(&V, 5));
}

/* D07 (review 3, finding 3): a second rebind of the same zone (Zigbee's, under its own hold)
 * while the first one's invalidation runs keeps the obligation: the invalidation clears only
 * the epoch it captured. */
static void test_second_rebind_during_invalidation_stays_owed(void)
{
    uint8_t owed[ALARM_ZONE_BITMAP_BYTES];
    TEST_ASSERT_TRUE(zone_voice_note_change(&V, 7));
    TEST_ASSERT_TRUE(zone_voice_owed(&V, owed));
    TEST_ASSERT_TRUE(zone_voice_note_change(&V, 7)); /* the second sensor, mid-invalidation */
    zone_voice_invalidated(&V, owed);                 /* stored for the first epoch only */
    TEST_ASSERT_TRUE(zone_voice_any_owed(&V));
    TEST_ASSERT_TRUE(zone_voice_settling(&V, 7));
    /* a store now would be refused (the service checks any_owed before it encodes) */
    TEST_ASSERT_TRUE(zone_voice_owed(&V, owed));
    TEST_ASSERT_TRUE(bit(owed, 7));
    zone_voice_invalidated(&V, owed);
    TEST_ASSERT_FALSE(zone_voice_any_owed(&V));
    zone_voice_capture(&V);
    zone_voice_stored(&V);
    TEST_ASSERT_FALSE(zone_voice_settling(&V, 7));
}

/* D07: a stale epoch never clears: a refused invalidation (not marked) stays owed, and a change
 * after the bindings were captured keeps the zone settling once that store lands. */
static void test_stale_epochs_never_clear(void)
{
    uint8_t owed[ALARM_ZONE_BITMAP_BYTES];
    TEST_ASSERT_TRUE(zone_voice_note_change(&V, 1));
    TEST_ASSERT_TRUE(zone_voice_owed(&V, owed)); /* refused: zone_voice_invalidated not called */
    TEST_ASSERT_TRUE(zone_voice_any_owed(&V));
    TEST_ASSERT_TRUE(zone_voice_owed(&V, owed));
    zone_voice_invalidated(&V, owed);
    zone_voice_capture(&V);
    TEST_ASSERT_TRUE(zone_voice_note_change(&V, 1)); /* between encode and the NVS write */
    zone_voice_stored(&V);
    TEST_ASSERT_TRUE(zone_voice_settling(&V, 1));
    TEST_ASSERT_TRUE(zone_voice_any_owed(&V));
    /* a failed store (not marked) keeps it settling too */
    TEST_ASSERT_TRUE(zone_voice_owed(&V, owed));
    zone_voice_invalidated(&V, owed);
    zone_voice_capture(&V);
    TEST_ASSERT_TRUE(zone_voice_settling(&V, 1));
    zone_voice_stored(&V);
    TEST_ASSERT_FALSE(zone_voice_settling(&V, 1));
}

/* D07: a change that didn't happen (a refused bind, a refused invalidation) is withdrawn, but
 * never over a later change of the same zone. */
static void test_undo_only_its_own_epoch(void)
{
    TEST_ASSERT_TRUE(zone_voice_note_change(&V, 3));
    uint32_t mine = zone_voice_epoch(&V, 3);
    TEST_ASSERT_TRUE(zone_voice_undo_change(&V, 3, mine));
    TEST_ASSERT_FALSE(zone_voice_settling(&V, 3));
    TEST_ASSERT_FALSE(zone_voice_any_owed(&V));
    TEST_ASSERT_TRUE(zone_voice_note_change(&V, 3));
    mine = zone_voice_epoch(&V, 3);
    TEST_ASSERT_TRUE(zone_voice_note_change(&V, 3)); /* another task's change meanwhile */
    TEST_ASSERT_FALSE(zone_voice_undo_change(&V, 3, mine));
    TEST_ASSERT_TRUE(zone_voice_settling(&V, 3));
    TEST_ASSERT_FALSE(zone_voice_undo_change(&V, 0, 0u));
    TEST_ASSERT_EQUAL_UINT32(0u, zone_voice_epoch(&V, ALARM_MAX_ZONES + 1u));
}

/* Out of range: nothing noted, never settling; the counters wrap without a false match. */
static void test_range_and_wrap(void)
{
    uint8_t owed[ALARM_ZONE_BITMAP_BYTES];
    TEST_ASSERT_FALSE(zone_voice_note_change(&V, 0));
    TEST_ASSERT_FALSE(zone_voice_note_change(&V, ALARM_MAX_ZONES + 1u));
    TEST_ASSERT_FALSE(zone_voice_settling(&V, 0));
    TEST_ASSERT_FALSE(zone_voice_settling(&V, ALARM_MAX_ZONES + 1u));
    TEST_ASSERT_FALSE(zone_voice_any_owed(&V));
    V.epoch[ALARM_MAX_ZONES - 1u] = UINT32_MAX;
    V.invalidated[ALARM_MAX_ZONES - 1u] = UINT32_MAX;
    V.stored[ALARM_MAX_ZONES - 1u] = UINT32_MAX;
    TEST_ASSERT_FALSE(zone_voice_settling(&V, ALARM_MAX_ZONES));
    TEST_ASSERT_TRUE(zone_voice_note_change(&V, ALARM_MAX_ZONES));
    TEST_ASSERT_TRUE(zone_voice_settling(&V, ALARM_MAX_ZONES));
    TEST_ASSERT_TRUE(zone_voice_owed(&V, owed));
    TEST_ASSERT_TRUE(bit(owed, ALARM_MAX_ZONES));
    zone_voice_invalidated(&V, owed);
    zone_voice_capture(&V);
    zone_voice_stored(&V);
    TEST_ASSERT_FALSE(zone_voice_settling(&V, ALARM_MAX_ZONES));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_one_change_settles_after_both_stores);
    RUN_TEST(test_second_rebind_during_invalidation_stays_owed);
    RUN_TEST(test_stale_epochs_never_clear);
    RUN_TEST(test_undo_only_its_own_epoch);
    RUN_TEST(test_range_and_wrap);
    return UNITY_END();
}
