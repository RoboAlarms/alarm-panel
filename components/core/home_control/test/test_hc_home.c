/* The house's Smart Home settings (docs/features/24, BL-220): the rooms the panel keeps, where a
 * person put each device, the devices that ask for a master code, and the stored blob read by later
 * firmware. Portable: nothing but the registry's types and Unity. */
#include "hc_home_fixture_v1.h"
#include "home_control/hc_home.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

void setUp(void);
void tearDown(void);
void setUp(void) {}
void tearDown(void) {}

static hc_home_t s_h;
static hc_home_t s_h2;
static uint8_t s_blob[HC_HOME_BLOB_MAX + 8u];

/* HCTL-014: version 1's frozen blob still reads, and written back it is the same bytes. */
static void test_fixture_v1_reads(void)
{
    TEST_ASSERT_TRUE(hc_home_decode(&s_h, HC_HOME_FIXTURE_V1, sizeof(HC_HOME_FIXTURE_V1)));
    TEST_ASSERT_TRUE(hc_home_arrange_code(&s_h));
    TEST_ASSERT_EQUAL_UINT8(2, s_h.nrooms);
    TEST_ASSERT_EQUAL_UINT8(3, s_h.rooms[0].id);
    TEST_ASSERT_EQUAL_STRING("Kitchen and dining", s_h.rooms[0].name);
    TEST_ASSERT_EQUAL_STRING("Kitchen", s_h.rooms[0].src);
    TEST_ASSERT_EQUAL_HEX8(HC_HOME_R_RENAMED, s_h.rooms[0].flags);
    TEST_ASSERT_EQUAL_STRING("Garage", s_h.rooms[1].name);
    TEST_ASSERT_EQUAL_STRING("", s_h.rooms[1].src);
    TEST_ASSERT_EQUAL_UINT8(2, s_h.ndevs);
    TEST_ASSERT_TRUE(hc_home_placed(&s_h, "climate.hall"));
    TEST_ASSERT_TRUE(hc_home_protected(&s_h, "climate.hall"));
    TEST_ASSERT_FALSE(hc_home_placed(&s_h, "lock.front"));
    TEST_ASSERT_TRUE(hc_home_protected(&s_h, "lock.front"));
    TEST_ASSERT_EQUAL_INT(1, hc_home_room_of(&s_h, "climate.hall", "Kitchen"));
    TEST_ASSERT_EQUAL_INT(0, hc_home_room_of(&s_h, "lock.front", "Kitchen"));
    const size_t len = hc_home_encode(&s_h, s_blob, sizeof(s_blob));
    TEST_ASSERT_EQUAL_size_t(HC_HOME_FIXTURE_V1_LEN, len);
    TEST_ASSERT_EQUAL_MEMORY(HC_HOME_FIXTURE_V1, s_blob, len);
}

/* A blob cut anywhere, or of a compat version 1 doesn't know, reads as the defaults. */
static void test_bad_blobs_read_as_defaults(void)
{
    for (size_t cut = 0; cut < HC_HOME_FIXTURE_V1_LEN; cut++) {
        TEST_ASSERT_FALSE(hc_home_decode(&s_h, HC_HOME_FIXTURE_V1, cut));
        TEST_ASSERT_EQUAL_UINT8(0, s_h.nrooms);
        TEST_ASSERT_EQUAL_UINT8(0, s_h.ndevs);
        TEST_ASSERT_FALSE(hc_home_arrange_code(&s_h));
    }
    uint8_t b[sizeof(HC_HOME_FIXTURE_V1)];
    memcpy(b, HC_HOME_FIXTURE_V1, sizeof(b));
    b[0] = 2u;
    TEST_ASSERT_FALSE(hc_home_decode(&s_h, b, sizeof(b)));
    TEST_ASSERT_FALSE(hc_home_decode(&s_h, NULL, 0));
}

/* The largest settings - every room and device, the longest names and refs - fit the bound. */
static void test_round_trip_at_the_bound(void)
{
    hc_home_clear(&s_h);
    s_h.flags = HC_HOME_F_ARRANGE_CODE;
    for (unsigned i = 0; i < HC_HOME_ROOMS; i++) {
        hc_home_room_t *r = &s_h.rooms[s_h.nrooms++];
        r->id = (uint8_t)(i + 1u);
        r->flags = HC_HOME_R_RENAMED;
        memset(r->name, 'a' + (int)i, HC_HOME_NAME_MAX);
        r->name[HC_HOME_NAME_MAX] = '\0';
        memset(r->src, 'A' + (int)i, HC_HOME_NAME_MAX);
        r->src[HC_HOME_NAME_MAX] = '\0';
    }
    for (unsigned i = 0; i < HC_HOME_DEVICES; i++) {
        hc_home_dev_t *d = &s_h.devs[s_h.ndevs++];
        d->flags = HC_HOME_D_PLACED | HC_HOME_D_PROTECTED;
        d->room = (uint8_t)(i % HC_HOME_ROOMS + 1u);
        memset(d->ref, 'a' + (int)(i % 26u), HC_HOME_REF_MAX);
        d->ref[0] = (char)('A' + (int)(i / 26u));
        d->ref[HC_HOME_REF_MAX] = '\0';
    }
    const size_t len = hc_home_encode(&s_h, s_blob, sizeof(s_blob));
    TEST_ASSERT_EQUAL_size_t(HC_HOME_BLOB_MAX, len);
    TEST_ASSERT_TRUE(hc_home_decode(&s_h2, s_blob, len));
    TEST_ASSERT_EQUAL_UINT8(HC_HOME_ROOMS, s_h2.nrooms);
    TEST_ASSERT_EQUAL_UINT8(HC_HOME_DEVICES, s_h2.ndevs);
    TEST_ASSERT_EQUAL_size_t(0u, hc_home_encode(&s_h, s_blob, HC_HOME_BLOB_MAX - 1u));
}

/* HCTL-014 (BL-220 R3): a device follows its source's area into a room made from it; a room of
 * that name serves too; a person's choice wins from then on, and following again forgets it. */
static void test_a_device_follows_its_source_until_placed(void)
{
    hc_home_clear(&s_h);
    const hc_home_seen_t seen[] = {{"light.porch", "Outside"}, {"lock.front", "Outside"},
                                   {"climate.hall", "Hallway"}, {"plug.pump", ""}};
    TEST_ASSERT_TRUE(hc_home_adopt(&s_h, seen, 4, true));
    TEST_ASSERT_EQUAL_UINT8(2, s_h.nrooms);
    TEST_ASSERT_EQUAL_STRING("Outside", s_h.rooms[0].name);
    TEST_ASSERT_EQUAL_STRING("Hallway", s_h.rooms[1].name);
    TEST_ASSERT_EQUAL_INT(0, hc_home_room_of(&s_h, "light.porch", "Outside"));
    TEST_ASSERT_EQUAL_INT(-1, hc_home_room_of(&s_h, "plug.pump", ""));
    TEST_ASSERT_FALSE(hc_home_adopt(&s_h, seen, 4, true)); /* nothing new */
    TEST_ASSERT_EQUAL_UINT8(0, s_h.ndevs);          /* following needs no entry */

    const uint8_t garage = hc_home_add(&s_h, "Garage");
    TEST_ASSERT_NOT_EQUAL(0, garage);
    TEST_ASSERT_TRUE(hc_home_place(&s_h, "light.porch", garage));
    TEST_ASSERT_EQUAL_INT(2, hc_home_room_of(&s_h, "light.porch", "Outside"));
    TEST_ASSERT_EQUAL_STRING("Garage", hc_home_room_name(&s_h, "light.porch", "Outside"));
    TEST_ASSERT_FALSE(hc_home_place(&s_h, "light.porch", garage)); /* already there */
    TEST_ASSERT_TRUE(hc_home_place(&s_h, "plug.pump", 0u));        /* no room, on purpose */
    TEST_ASSERT_EQUAL_STRING("", hc_home_room_name(&s_h, "plug.pump", ""));

    TEST_ASSERT_TRUE(hc_home_follow(&s_h, "light.porch"));
    TEST_ASSERT_EQUAL_INT(0, hc_home_room_of(&s_h, "light.porch", "Outside"));
    TEST_ASSERT_EQUAL_INT(-1, hc_home_dev_index(&s_h, "light.porch")); /* nothing left to keep */
    /* a room a person named as an area takes that area's devices */
    hc_home_clear(&s_h);
    TEST_ASSERT_NOT_EQUAL(0, hc_home_add(&s_h, "outside"));
    TEST_ASSERT_FALSE(hc_home_adopt(&s_h, seen, 1, true));
    TEST_ASSERT_EQUAL_INT(0, hc_home_room_of(&s_h, "light.porch", "Outside"));
}

/* HCTL-014 (BL-220 R3): a room made from an area goes when nothing is in it, unless a person
 * renamed it; a renamed one keeps its link, so the source can't bring the old name back as a second
 * room. */
static void test_area_rooms_come_and_go(void)
{
    hc_home_clear(&s_h);
    hc_home_seen_t seen[] = {{"light.k", "Kitchen"}, {"light.l", "Living"}};
    TEST_ASSERT_TRUE(hc_home_adopt(&s_h, seen, 2, true));
    seen[1].area = "Kitchen"; /* the source moved it: Living is empty */
    TEST_ASSERT_FALSE(hc_home_adopt(&s_h, seen, 2, false)); /* kept until Smart Home opens */
    TEST_ASSERT_EQUAL_UINT8(2, s_h.nrooms);
    TEST_ASSERT_TRUE(hc_home_adopt(&s_h, seen, 2, true));
    TEST_ASSERT_EQUAL_UINT8(1, s_h.nrooms);
    TEST_ASSERT_TRUE(hc_home_rename(&s_h, s_h.rooms[0].id, "Kitchen and dining"));
    TEST_ASSERT_FALSE(hc_home_adopt(&s_h, seen, 2, true));
    TEST_ASSERT_EQUAL_UINT8(1, s_h.nrooms);
    TEST_ASSERT_EQUAL_STRING("Kitchen and dining", hc_home_room_name(&s_h, "light.k", "Kitchen"));
    seen[0].area = "";
    seen[1].area = "";
    TEST_ASSERT_FALSE(hc_home_adopt(&s_h, seen, 2, true)); /* renamed: it stays, empty */
    TEST_ASSERT_EQUAL_UINT8(1, s_h.nrooms);
    TEST_ASSERT_FALSE(hc_home_adopt(&s_h, seen, 0, true)); /* an empty registry changes nothing */
}

/* HCTL-014 (BL-220 R2): removing a room leaves its devices on the panel in no room, the ones that
 * followed its area too - the source doesn't put them back - and nothing else changes. */
static void test_removing_a_room_keeps_its_devices(void)
{
    hc_home_clear(&s_h);
    const hc_home_seen_t seen[] = {{"light.k", "Kitchen"}, {"plug.k", ""}, {"light.o", "Outside"}};
    TEST_ASSERT_TRUE(hc_home_adopt(&s_h, seen, 3, true));
    const uint8_t kitchen = s_h.rooms[0].id;
    TEST_ASSERT_TRUE(hc_home_place(&s_h, "plug.k", kitchen));
    TEST_ASSERT_TRUE(hc_home_protect(&s_h, "light.o", true));
    TEST_ASSERT_TRUE(hc_home_remove(&s_h, kitchen, seen, 3));
    TEST_ASSERT_EQUAL_UINT8(1, s_h.nrooms);
    TEST_ASSERT_EQUAL_INT(-1, hc_home_room_of(&s_h, "light.k", "Kitchen"));
    TEST_ASSERT_EQUAL_INT(-1, hc_home_room_of(&s_h, "plug.k", ""));
    TEST_ASSERT_FALSE(hc_home_adopt(&s_h, seen, 3, true)); /* Kitchen doesn't come back */
    TEST_ASSERT_EQUAL_INT(0, hc_home_room_of(&s_h, "light.o", "Outside"));
    TEST_ASSERT_TRUE(hc_home_protected(&s_h, "light.o"));
    TEST_ASSERT_FALSE(hc_home_remove(&s_h, kitchen, seen, 3));
    /* a device new to the panel in that area later brings a room of that name back */
    const hc_home_seen_t more[] = {
        {"light.k", "Kitchen"}, {"light.o", "Outside"}, {"light.new", "Kitchen"}};
    TEST_ASSERT_TRUE(hc_home_adopt(&s_h, more, 3, true));
    TEST_ASSERT_EQUAL_INT(-1, hc_home_room_of(&s_h, "light.k", "Kitchen"));
    TEST_ASSERT_EQUAL_INT(1, hc_home_room_of(&s_h, "light.new", "Kitchen"));
}

/* HCTL-014 (BL-220 R2): names are trimmed, up to 47 bytes on a whole character, never empty and
 * never another room's (case aside); a room moves one tab at a time; ids are never reused while
 * kept. */
static void test_names_order_and_ids(void)
{
    hc_home_clear(&s_h);
    TEST_ASSERT_EQUAL_UINT8(0, hc_home_add(&s_h, "   "));
    const uint8_t a = hc_home_add(&s_h, "  Kitchen ");
    TEST_ASSERT_EQUAL_STRING("Kitchen", s_h.rooms[0].name);
    TEST_ASSERT_EQUAL(HC_HOME_NAME_TAKEN, hc_home_name_check(&s_h, "KITCHEN", 0u));
    TEST_ASSERT_EQUAL(HC_HOME_NAME_OK, hc_home_name_check(&s_h, "kitchen", a)); /* its own */
    TEST_ASSERT_EQUAL_UINT8(0, hc_home_add(&s_h, "kitchen"));
    const uint8_t b = hc_home_add(&s_h, "Garage");
    TEST_ASSERT_TRUE(b > a);
    TEST_ASSERT_FALSE(hc_home_rename(&s_h, b, "Kitchen"));
    TEST_ASSERT_TRUE(hc_home_rename(&s_h, b, "Garage and shed"));
    TEST_ASSERT_TRUE(hc_home_move(&s_h, b, -1));
    TEST_ASSERT_EQUAL_UINT8(b, s_h.rooms[0].id);
    TEST_ASSERT_FALSE(hc_home_move(&s_h, b, -1));
    TEST_ASSERT_FALSE(hc_home_move(&s_h, a, +1));
    /* 47 bytes, cut on a whole character: 46 ASCII then a two-byte one */
    char longname[64];
    memset(longname, 'x', 46);
    memcpy(&longname[46], "\xC3\xA9\xC3\xA9", 5);
    const uint8_t c = hc_home_add(&s_h, longname);
    TEST_ASSERT_NOT_EQUAL(0, c);
    TEST_ASSERT_EQUAL_size_t(46u, strlen(s_h.rooms[hc_home_room_index(&s_h, c)].name));
    /* full */
    for (unsigned i = s_h.nrooms; i < HC_HOME_ROOMS; i++) {
        char name[16];
        snprintf(name, sizeof(name), "Room %u", i);
        TEST_ASSERT_NOT_EQUAL(0, hc_home_add(&s_h, name));
    }
    TEST_ASSERT_EQUAL(HC_HOME_NAME_FULL, hc_home_name_check(&s_h, "One more", 0u));
}

/* HCTL-015 (BL-220 R5): which devices ask for a master code, and Arrange's switch; a device with
 * nothing left to keep leaves the list; the entries of devices gone from the registry are
 * forgotten. */
static void test_protection_and_prune(void)
{
    hc_home_clear(&s_h);
    TEST_ASSERT_FALSE(hc_home_protected(&s_h, "climate.hall"));
    TEST_ASSERT_TRUE(hc_home_protect(&s_h, "climate.hall", true));
    TEST_ASSERT_FALSE(hc_home_protect(&s_h, "climate.hall", true));
    TEST_ASSERT_TRUE(hc_home_protected(&s_h, "climate.hall"));
    TEST_ASSERT_TRUE(hc_home_protect(&s_h, "climate.hall", false));
    TEST_ASSERT_EQUAL_UINT8(0, s_h.ndevs);
    TEST_ASSERT_TRUE(hc_home_set_arrange_code(&s_h, true));
    TEST_ASSERT_TRUE(hc_home_arrange_code(&s_h));
    TEST_ASSERT_TRUE(hc_home_protect(&s_h, "a", true));
    TEST_ASSERT_TRUE(hc_home_protect(&s_h, "b", true));
    const hc_home_seen_t seen[] = {{"b", ""}};
    TEST_ASSERT_FALSE(hc_home_prune(&s_h, seen, 0));
    TEST_ASSERT_TRUE(hc_home_prune(&s_h, seen, 1));
    TEST_ASSERT_EQUAL_UINT8(1, s_h.ndevs);
    TEST_ASSERT_TRUE(hc_home_protected(&s_h, "b"));
}

/* HCTL-013, HCTL-014: a screen that knows no rooms gets each device's room name and its tab's
 * order. */
static void test_names_and_ranks_for_a_keypad(void)
{
    hc_home_clear(&s_h);
    const uint8_t k = hc_home_add(&s_h, "Kitchen");
    const uint8_t g = hc_home_add(&s_h, "Garage");
    (void)k;
    TEST_ASSERT_TRUE(hc_home_place(&s_h, "a", g));
    TEST_ASSERT_EQUAL_UINT(1u, hc_home_room_rank(&s_h, "a", "Kitchen"));
    TEST_ASSERT_EQUAL_UINT(0u, hc_home_room_rank(&s_h, "b", "kitchen"));
    TEST_ASSERT_EQUAL_STRING("Patio", hc_home_room_name(&s_h, "c", "Patio")); /* no room yet */
    TEST_ASSERT_EQUAL_UINT(HC_HOME_ROOMS, hc_home_room_rank(&s_h, "c", "Patio"));
    TEST_ASSERT_EQUAL_UINT(HC_HOME_ROOMS + 1u, hc_home_room_rank(&s_h, "d", ""));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fixture_v1_reads);
    RUN_TEST(test_bad_blobs_read_as_defaults);
    RUN_TEST(test_round_trip_at_the_bound);
    RUN_TEST(test_a_device_follows_its_source_until_placed);
    RUN_TEST(test_area_rooms_come_and_go);
    RUN_TEST(test_removing_a_room_keeps_its_devices);
    RUN_TEST(test_names_order_and_ids);
    RUN_TEST(test_protection_and_prune);
    RUN_TEST(test_names_and_ranks_for_a_keypad);
    return UNITY_END();
}
