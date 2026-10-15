/* The Smart Home screen's model (docs/features/24, BL-045): the layout blob - a stored format, read
 * by later firmware - and the defaults when nothing is stored (HCTL-003, HCTL-008), and the source's
 * state words. Portable: nothing but the contract and Unity. */
#include "ui_backend/ui_dev_model.h"
#include "ui_dev_layout_fixture_v1.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

void setUp(void);
void tearDown(void);
void setUp(void) {}
void tearDown(void) {}

static ui_devl_t s_l;
static ui_devl_t s_l2;
static ui_dev_t s_many[UI_DEVL_MAX + 1u]; /* a registry one larger than the layout holds */
static uint8_t s_blob[UI_DEVL_BLOB_MAX + 8u];

static ui_dev_t dev(const char *ref, uint8_t kind, uint8_t caps, const char *area)
{
    ui_dev_t d;
    memset(&d, 0, sizeof(d));
    d.kind = kind;
    d.caps = caps;
    d.level = -1;
    snprintf(d.ref, sizeof(d.ref), "%s", ref);
    snprintf(d.name, sizeof(d.name), "%s", ref);
    snprintf(d.area, sizeof(d.area), "%s", area);
    return d;
}

/* HCTL-008: version 1's frozen blob still reads, entry for entry, and the bytes a later version
 * appends after the entries are ignored; written back, it is the same bytes. */
static void test_fixture_v1_reads(void)
{
    TEST_ASSERT_TRUE(ui_devl_decode(&s_l, UI_DEV_LAYOUT_FIXTURE_V1, sizeof(UI_DEV_LAYOUT_FIXTURE_V1)));
    TEST_ASSERT_TRUE(s_l.stored);
    TEST_ASSERT_EQUAL_UINT8(3, s_l.n);
    TEST_ASSERT_EQUAL_STRING("light.porch", s_l.e[0].ref);
    TEST_ASSERT_EQUAL_HEX8(UI_DEVL_FAV | (UI_DEVL_SIZE_WIDE << UI_DEVL_SIZE_SHIFT), s_l.e[0].flags);
    TEST_ASSERT_EQUAL_STRING("lock.front_door", s_l.e[1].ref);
    TEST_ASSERT_EQUAL_HEX8(UI_DEVL_FAV, s_l.e[1].flags);
    TEST_ASSERT_EQUAL_STRING("cover.garage", s_l.e[2].ref);
    TEST_ASSERT_EQUAL_HEX8(UI_DEVL_SIZE_SMALL << UI_DEVL_SIZE_SHIFT, s_l.e[2].flags);
    const size_t len = ui_devl_encode(&s_l, s_blob, sizeof(s_blob));
    TEST_ASSERT_EQUAL_size_t(UI_DEV_LAYOUT_FIXTURE_V1_ENTRIES_LEN, len);
    TEST_ASSERT_EQUAL_MEMORY(UI_DEV_LAYOUT_FIXTURE_V1, s_blob, len);
}

/* HCTL-008: the largest layout - every device, the longest refs - fits the bound and comes back. */
static void test_round_trip_at_the_bound(void)
{
    ui_devl_clear(&s_l);
    for (unsigned i = 0; i < UI_DEVL_MAX; i++) {
        memset(s_l.e[i].ref, 'a' + (int)(i % 26u), UI_DEVL_REF_MAX);
        s_l.e[i].ref[0] = (char)('A' + (int)(i / 26u));
        s_l.e[i].ref[UI_DEVL_REF_MAX] = '\0';
        s_l.e[i].flags = (uint8_t)(i % 2u ? UI_DEVL_FAV : (UI_DEVL_SIZE_WIDE << UI_DEVL_SIZE_SHIFT));
    }
    s_l.n = UI_DEVL_MAX;
    const size_t len = ui_devl_encode(&s_l, s_blob, sizeof(s_blob));
    TEST_ASSERT_EQUAL_size_t(UI_DEVL_BLOB_MAX, len);
    TEST_ASSERT_TRUE(UI_DEVL_BLOB_MAX <= 6400u);
    TEST_ASSERT_TRUE(ui_devl_decode(&s_l2, s_blob, len));
    TEST_ASSERT_EQUAL_UINT8(UI_DEVL_MAX, s_l2.n);
    for (unsigned i = 0; i < UI_DEVL_MAX; i++) {
        TEST_ASSERT_EQUAL_STRING(s_l.e[i].ref, s_l2.e[i].ref);
        TEST_ASSERT_EQUAL_HEX8(s_l.e[i].flags, s_l2.e[i].flags);
    }
    /* One byte short of room: nothing is written rather than half a layout. */
    TEST_ASSERT_EQUAL_size_t(0, ui_devl_encode(&s_l, s_blob, len - 1u));
    TEST_ASSERT_EQUAL_size_t(0, ui_devl_encode(&s_l, s_blob, 1u));
}

/* HCTL-008: garbage, a cut-off blob or a newer incompatible one is refused as a whole and the
 * screen falls back to its defaults (stored false). */
static void test_garbage_is_refused(void)
{
    static const uint8_t zero[] = {0x00, 0x00};
    static const uint8_t newer[] = {0x02, 0x00};
    static const uint8_t too_many[] = {0x01, UI_DEVL_MAX + 1u};
    static const uint8_t empty_ref[] = {0x01, 0x01, 0x01, 0x00};
    static const uint8_t long_ref[] = {0x01, 0x01, 0x01, DEV_REF_LEN};
    static const uint8_t nul_ref[] = {0x01, 0x01, 0x01, 0x03, 'a', 0x00, 'b'};
    TEST_ASSERT_FALSE(ui_devl_decode(&s_l, NULL, 0));
    TEST_ASSERT_FALSE(ui_devl_decode(&s_l, UI_DEV_LAYOUT_FIXTURE_V1, 1));
    TEST_ASSERT_FALSE(ui_devl_decode(&s_l, zero, sizeof(zero)));
    TEST_ASSERT_FALSE(ui_devl_decode(&s_l, newer, sizeof(newer)));
    TEST_ASSERT_FALSE(ui_devl_decode(&s_l, too_many, sizeof(too_many)));
    TEST_ASSERT_FALSE(ui_devl_decode(&s_l, empty_ref, sizeof(empty_ref)));
    TEST_ASSERT_FALSE(ui_devl_decode(&s_l, long_ref, sizeof(long_ref)));
    TEST_ASSERT_FALSE(ui_devl_decode(&s_l, nul_ref, sizeof(nul_ref)));
    TEST_ASSERT_FALSE(s_l.stored);
    TEST_ASSERT_EQUAL_UINT8(0, s_l.n);
    /* Cut anywhere inside its entries, the fixture is refused. */
    for (size_t cut = 2; cut < UI_DEV_LAYOUT_FIXTURE_V1_ENTRIES_LEN; cut++) {
        TEST_ASSERT_FALSE_MESSAGE(ui_devl_decode(&s_l, UI_DEV_LAYOUT_FIXTURE_V1, cut), "cut");
        TEST_ASSERT_FALSE(s_l.stored);
    }
    /* An empty layout is a layout: nothing on Favorites. */
    static const uint8_t none[] = {0x01, 0x00};
    TEST_ASSERT_TRUE(ui_devl_decode(&s_l, none, sizeof(none)));
    TEST_ASSERT_TRUE(s_l.stored);
}

/* HCTL-008: reserved flag bits a later version sets are ignored, and a ref named twice keeps its
 * first place. */
static void test_reserved_bits_and_duplicates(void)
{
    static const uint8_t blob[] = {0x01, 0x03, 0xF9, 0x01, 'a', 0x04, 0x01, 'b', 0x05, 0x01, 'a'};
    TEST_ASSERT_TRUE(ui_devl_decode(&s_l, blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_UINT8(2, s_l.n);
    TEST_ASSERT_EQUAL_STRING("a", s_l.e[0].ref);
    TEST_ASSERT_EQUAL_HEX8(UI_DEVL_FAV, s_l.e[0].flags);
    TEST_ASSERT_EQUAL_STRING("b", s_l.e[1].ref);
    TEST_ASSERT_EQUAL_HEX8(UI_DEVL_SIZE_WIDE << UI_DEVL_SIZE_SHIFT, s_l.e[1].flags);
    /* An entry without a ref is never written. */
    s_l.e[1].ref[0] = '\0';
    TEST_ASSERT_EQUAL_size_t(0, ui_devl_encode(&s_l, s_blob, sizeof(s_blob)));
}

/* HCTL-003, HCTL-008: nothing stored - every device on Favorites in the registry's order, two cells
 * where the kind carries a control; an area page holds its own devices. */
static void test_defaults(void)
{
    const ui_dev_t devs[] = {
        dev("light.porch", DEV_LIGHT, UI_DEV_CAP_LEVEL, "Outside"),
        dev("lock.front", DEV_LOCK, 0, "Hall"),
        dev("climate.hall", DEV_CLIMATE, 0, "Hall"),
        dev("switch.pump", DEV_SWITCH, 0, ""),
        dev("light.kitchen", DEV_LIGHT, 0, "Kitchen"),
        dev("cover.awning", DEV_COVER, UI_DEV_CAP_LEVEL, "Outside"),
        dev("cover.garage", DEV_COVER, UI_DEV_CAP_ENTRANCE, "Outside"),
        dev("fan.porch", DEV_FAN, 0, "Outside"),
        dev("scene.night", DEV_SCENE, 0, "Hall"),
    };
    const size_t n = sizeof(devs) / sizeof(devs[0]);
    uint8_t page[16];
    ui_devl_clear(&s_l);
    TEST_ASSERT_EQUAL_size_t(n, ui_devl_page(&s_l, devs, n, NULL, page, sizeof(page)));
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT8(i, page[i]);
        TEST_ASSERT_TRUE(ui_devl_favorite(&s_l, &devs[i]));
    }
    static const bool wide[] = {true, false, true, false, false, true, false, true, false};
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(wide[i], ui_devl_wide(&s_l, &devs[i]), devs[i].ref);
    }
    TEST_ASSERT_EQUAL_size_t(4, ui_devl_page(&s_l, devs, n, "Outside", page, sizeof(page)));
    TEST_ASSERT_EQUAL_UINT8(0, page[0]);
    TEST_ASSERT_EQUAL_UINT8(5, page[1]);
    TEST_ASSERT_EQUAL_UINT8(6, page[2]);
    TEST_ASSERT_EQUAL_UINT8(7, page[3]);
    TEST_ASSERT_EQUAL_size_t(1, ui_devl_page(&s_l, devs, n, "", page, sizeof(page)));
    TEST_ASSERT_EQUAL_UINT8(3, page[0]);
    /* max bounds what is written */
    TEST_ASSERT_EQUAL_size_t(2, ui_devl_page(&s_l, devs, n, NULL, page, 2));
}

/* HCTL-008: a stored layout - Favorites holds the flagged ones in its order, an area page the
 * layout's order first and the devices it doesn't name after them; a size set overrides the kind's,
 * and a device gone from the registry is skipped. */
static void test_stored_layout(void)
{
    const ui_dev_t devs[] = {
        dev("light.porch", DEV_LIGHT, UI_DEV_CAP_LEVEL, "Outside"),
        dev("lock.front", DEV_LOCK, 0, "Hall"),
        dev("cover.garage", DEV_COVER, 0, "Outside"),
        dev("switch.pump", DEV_SWITCH, 0, "Outside"),
    };
    const size_t n = sizeof(devs) / sizeof(devs[0]);
    static const uint8_t blob[] = {
        0x01, 0x04,
        0x01, 10, 'l', 'o', 'c', 'k', '.', 'f', 'r', 'o', 'n', 't',
        0x01, 9, 'l', 'i', 'g', 'h', 't', '.', 'o', 'l', 'd',
        0x03, 11, 'l', 'i', 'g', 'h', 't', '.', 'p', 'o', 'r', 'c', 'h',
        0x04, 12, 'c', 'o', 'v', 'e', 'r', '.', 'g', 'a', 'r', 'a', 'g', 'e',
    };
    TEST_ASSERT_TRUE(ui_devl_decode(&s_l, blob, sizeof(blob)));
    uint8_t page[8];
    TEST_ASSERT_EQUAL_size_t(2, ui_devl_page(&s_l, devs, n, NULL, page, sizeof(page)));
    TEST_ASSERT_EQUAL_UINT8(1, page[0]); /* lock.front */
    TEST_ASSERT_EQUAL_UINT8(0, page[1]); /* light.porch; light.old is gone */
    TEST_ASSERT_FALSE(ui_devl_favorite(&s_l, &devs[2]));
    TEST_ASSERT_FALSE(ui_devl_favorite(&s_l, &devs[3])); /* not named: not on Favorites once stored */
    TEST_ASSERT_FALSE(ui_devl_wide(&s_l, &devs[0])); /* set small */
    TEST_ASSERT_TRUE(ui_devl_wide(&s_l, &devs[2]));  /* set wide */
    TEST_ASSERT_FALSE(ui_devl_wide(&s_l, &devs[3])); /* the kind's */
    TEST_ASSERT_EQUAL_size_t(3, ui_devl_page(&s_l, devs, n, "Outside", page, sizeof(page)));
    TEST_ASSERT_EQUAL_UINT8(0, page[0]);
    TEST_ASSERT_EQUAL_UINT8(2, page[1]);
    TEST_ASSERT_EQUAL_UINT8(3, page[2]);
}

static size_t page_refs(const ui_dev_t *devs, size_t n, const char *area, char out[][DEV_REF_LEN])
{
    uint8_t page[UI_DEVL_MAX];
    const size_t k = ui_devl_page(&s_l, devs, n, area, page, sizeof(page));
    for (size_t i = 0; i < k; i++) {
        snprintf(out[i], DEV_REF_LEN, "%s", devs[page[i]].ref);
    }
    return k;
}

/* HCTL-008: Arrange's edits - move a place earlier or later, small or wide, off Favorites and back
 * at the end - starting from the defaults, kept in the stored format and bounded. */
static void test_arrange_edits(void)
{
    const ui_dev_t devs[] = {
        dev("light.porch", DEV_LIGHT, UI_DEV_CAP_LEVEL, "Outside"),
        dev("lock.front", DEV_LOCK, 0, "Hall"),
        dev("lock.back", DEV_LOCK, 0, "Hall"),
        dev("fan.porch", DEV_FAN, 0, "Outside"),
    };
    const size_t n = sizeof(devs) / sizeof(devs[0]);
    char refs[UI_DEVL_MAX][DEV_REF_LEN];
    ui_devl_clear(&s_l);
    /* moving the defaults writes them out, then swaps with the neighbour */
    TEST_ASSERT_TRUE(ui_devl_move(&s_l, devs, n, NULL, "light.porch", 1));
    TEST_ASSERT_TRUE(s_l.stored);
    TEST_ASSERT_EQUAL_size_t(4, page_refs(devs, n, NULL, refs));
    TEST_ASSERT_EQUAL_STRING("lock.front", refs[0]);
    TEST_ASSERT_EQUAL_STRING("light.porch", refs[1]);
    TEST_ASSERT_FALSE(ui_devl_move(&s_l, devs, n, NULL, "lock.front", -1)); /* the first: no earlier */
    TEST_ASSERT_FALSE(ui_devl_move(&s_l, devs, n, NULL, "fan.porch", 1));   /* the last: no later */
    TEST_ASSERT_FALSE(ui_devl_move(&s_l, devs, n, "Hall", "light.porch", 1)); /* not on that page */
    TEST_ASSERT_FALSE(ui_devl_move(&s_l, devs, n, NULL, "lock.front", 0));    /* no step */
    TEST_ASSERT_FALSE(ui_devl_move(&s_l, devs, n, NULL, "light.gone", 1));    /* not a device */
    TEST_ASSERT_TRUE(ui_devl_remove(&s_l, devs, n, "light.gone")); /* never on Favorites */
    TEST_ASSERT_EQUAL_UINT8(4, s_l.n);
    /* size: the kind's until set; the favourite flag stays */
    TEST_ASSERT_FALSE(ui_devl_wide(&s_l, &devs[1]));
    TEST_ASSERT_TRUE(ui_devl_set_size(&s_l, devs, n, "lock.front", UI_DEVL_SIZE_WIDE));
    TEST_ASSERT_TRUE(ui_devl_wide(&s_l, &devs[1]));
    TEST_ASSERT_TRUE(ui_devl_favorite(&s_l, &devs[1]));
    TEST_ASSERT_TRUE(ui_devl_set_size(&s_l, devs, n, "fan.porch", UI_DEVL_SIZE_SMALL));
    TEST_ASSERT_FALSE(ui_devl_wide(&s_l, &devs[3]));
    /* off Favorites: gone from it, still on its area's page in its place */
    TEST_ASSERT_TRUE(ui_devl_remove(&s_l, devs, n, "lock.back"));
    TEST_ASSERT_EQUAL_size_t(3, page_refs(devs, n, NULL, refs));
    TEST_ASSERT_EQUAL_size_t(2, page_refs(devs, n, "Hall", refs));
    TEST_ASSERT_EQUAL_STRING("lock.front", refs[0]);
    TEST_ASSERT_EQUAL_STRING("lock.back", refs[1]);
    /* an area page moves too, and the order holds on Favorites */
    TEST_ASSERT_TRUE(ui_devl_move(&s_l, devs, n, "Hall", "lock.back", -1));
    TEST_ASSERT_EQUAL_size_t(2, page_refs(devs, n, "Hall", refs));
    TEST_ASSERT_EQUAL_STRING("lock.back", refs[0]);
    /* added: on Favorites again at the end, at its kind's size */
    TEST_ASSERT_TRUE(ui_devl_add(&s_l, devs, n, "lock.back"));
    TEST_ASSERT_EQUAL_size_t(4, page_refs(devs, n, NULL, refs));
    TEST_ASSERT_EQUAL_STRING("lock.back", refs[3]);
    TEST_ASSERT_TRUE(ui_devl_favorite(&s_l, &devs[2]));
    TEST_ASSERT_TRUE(ui_devl_set_size(&s_l, devs, n, "light.porch", UI_DEVL_SIZE_SMALL));
    TEST_ASSERT_TRUE(ui_devl_add(&s_l, devs, n, "light.porch")); /* an add resets the size */
    TEST_ASSERT_TRUE(ui_devl_wide(&s_l, &devs[0]));
    /* an area page names the devices it shows: an unnamed one is placed, not lost */
    ui_devl_clear(&s_l);
    TEST_ASSERT_TRUE(ui_devl_name(&s_l, "lock.front", UI_DEVL_FAV));
    TEST_ASSERT_TRUE(ui_devl_move(&s_l, devs, n, "Hall", "lock.back", -1));
    TEST_ASSERT_EQUAL_size_t(2, page_refs(devs, n, "Hall", refs));
    TEST_ASSERT_EQUAL_STRING("lock.back", refs[0]);
    /* the edited layout writes and reads back */
    const size_t len = ui_devl_encode(&s_l, s_blob, sizeof(s_blob));
    TEST_ASSERT_TRUE(len > 0u);
    TEST_ASSERT_TRUE(ui_devl_decode(&s_l2, s_blob, len));
    TEST_ASSERT_EQUAL_UINT8(s_l.n, s_l2.n);
    /* bounded: a full layout names no more */
    ui_devl_clear(&s_l);
    for (unsigned i = 0; i < UI_DEVL_MAX; i++) {
        char ref[24];
        snprintf(ref, sizeof(ref), "switch.s%u", i);
        TEST_ASSERT_TRUE(ui_devl_name(&s_l, ref, 0u));
    }
    TEST_ASSERT_FALSE(ui_devl_name(&s_l, "switch.extra", 0u));
    TEST_ASSERT_EQUAL_UINT8(UI_DEVL_MAX, s_l.n);
    /* full of devices that have left: an edit forgets them first, and the add fits */
    TEST_ASSERT_TRUE(ui_devl_add(&s_l, devs, n, "lock.back"));
    TEST_ASSERT_EQUAL_UINT8(1, s_l.n);
    TEST_ASSERT_TRUE(ui_devl_favorite(&s_l, &devs[2]));
    /* full of devices still there: nothing is forgotten and every edit that needs a place fails */
    ui_devl_clear(&s_l);
    for (unsigned i = 0; i <= UI_DEVL_MAX; i++) {
        char ref[24];
        snprintf(ref, sizeof(ref), "switch.s%u", i);
        s_many[i] = dev(ref, DEV_SWITCH, 0, i == UI_DEVL_MAX ? "Attic" : "Hall");
        if (i < UI_DEVL_MAX) {
            TEST_ASSERT_TRUE(ui_devl_name(&s_l, ref, UI_DEVL_FAV));
        }
    }
    const size_t many = UI_DEVL_MAX + 1u;
    char last[24];
    snprintf(last, sizeof(last), "switch.s%u", (unsigned)UI_DEVL_MAX);
    TEST_ASSERT_FALSE(ui_devl_add(&s_l, s_many, many, last));
    TEST_ASSERT_FALSE(ui_devl_set_size(&s_l, s_many, many, last, UI_DEVL_SIZE_WIDE));
    s_many[0] = dev("switch.s0", DEV_SWITCH, 0, "Attic"); /* now beside the unnamed one */
    TEST_ASSERT_FALSE(ui_devl_move(&s_l, s_many, many, "Attic", last, -1));
    TEST_ASSERT_EQUAL_UINT8(UI_DEVL_MAX, s_l.n);
}

/* HCTL-008: a tile dragged onto another takes its place, the ones between shifting one place
 * towards where it was - later and earlier, on Favorites (from the defaults) and on an area's page
 * whose devices the layout doesn't name yet - the size and the favourite flag going with it, the
 * other pages' devices staying put, the result stored and read back; refused when nothing would
 * change or the place isn't on the page. */
static void test_arrange_move_to(void)
{
    const ui_dev_t devs[] = {
        dev("a", DEV_SWITCH, 0, "Hall"),   dev("b", DEV_LIGHT, UI_DEV_CAP_LEVEL, "Out"),
        dev("c", DEV_SWITCH, 0, "Hall"),   dev("d", DEV_LOCK, 0, "Out"),
        dev("e", DEV_SWITCH, 0, "Hall"),
    };
    const size_t n = sizeof(devs) / sizeof(devs[0]);
    char refs[UI_DEVL_MAX][DEV_REF_LEN];
    ui_devl_clear(&s_l);
    /* the defaults are written out first; a to d's place: b, c, d shift earlier */
    TEST_ASSERT_TRUE(ui_devl_set_size(&s_l, devs, n, "a", UI_DEVL_SIZE_WIDE));
    TEST_ASSERT_TRUE(ui_devl_move_to(&s_l, devs, n, NULL, "a", 3));
    TEST_ASSERT_EQUAL_size_t(5, page_refs(devs, n, NULL, refs));
    TEST_ASSERT_EQUAL_STRING("b", refs[0]);
    TEST_ASSERT_EQUAL_STRING("c", refs[1]);
    TEST_ASSERT_EQUAL_STRING("d", refs[2]);
    TEST_ASSERT_EQUAL_STRING("a", refs[3]);
    TEST_ASSERT_EQUAL_STRING("e", refs[4]);
    TEST_ASSERT_TRUE(ui_devl_wide(&s_l, &devs[0])); /* its size goes with it */
    TEST_ASSERT_TRUE(ui_devl_favorite(&s_l, &devs[0]));
    /* e to the first place: everything else one later */
    TEST_ASSERT_TRUE(ui_devl_move_to(&s_l, devs, n, NULL, "e", 0));
    TEST_ASSERT_EQUAL_size_t(5, page_refs(devs, n, NULL, refs));
    TEST_ASSERT_EQUAL_STRING("e", refs[0]);
    TEST_ASSERT_EQUAL_STRING("b", refs[1]);
    TEST_ASSERT_EQUAL_STRING("a", refs[4]);
    /* one place is the arrows' move */
    TEST_ASSERT_TRUE(ui_devl_move_to(&s_l, devs, n, NULL, "b", 2));
    TEST_ASSERT_EQUAL_size_t(5, page_refs(devs, n, NULL, refs));
    TEST_ASSERT_EQUAL_STRING("c", refs[1]);
    TEST_ASSERT_EQUAL_STRING("b", refs[2]);
    /* refused, changing nothing: its own place, past the end, not on the page, not a device */
    const ui_devl_t before = s_l;
    TEST_ASSERT_FALSE(ui_devl_move_to(&s_l, devs, n, NULL, "b", 2));
    TEST_ASSERT_FALSE(ui_devl_move_to(&s_l, devs, n, NULL, "b", 5));
    TEST_ASSERT_FALSE(ui_devl_move_to(&s_l, devs, n, NULL, "b", (size_t)-1));
    TEST_ASSERT_FALSE(ui_devl_move_to(&s_l, devs, n, "Hall", "b", 0));
    TEST_ASSERT_FALSE(ui_devl_move_to(&s_l, devs, n, NULL, "gone", 0));
    TEST_ASSERT_EQUAL_MEMORY(&before, &s_l, sizeof(s_l));
    /* off Favorites, b still moves on its area's page, and Favorites keeps its order */
    TEST_ASSERT_TRUE(ui_devl_remove(&s_l, devs, n, "d"));
    TEST_ASSERT_EQUAL_size_t(2, page_refs(devs, n, "Out", refs));
    TEST_ASSERT_EQUAL_STRING("b", refs[0]);
    TEST_ASSERT_TRUE(ui_devl_move_to(&s_l, devs, n, "Out", "b", 1));
    TEST_ASSERT_EQUAL_size_t(2, page_refs(devs, n, "Out", refs));
    TEST_ASSERT_EQUAL_STRING("d", refs[0]);
    TEST_ASSERT_EQUAL_STRING("b", refs[1]);
    TEST_ASSERT_EQUAL_size_t(4, page_refs(devs, n, NULL, refs));
    TEST_ASSERT_EQUAL_STRING("e", refs[0]);
    TEST_ASSERT_EQUAL_STRING("c", refs[1]);
    TEST_ASSERT_EQUAL_STRING("b", refs[2]);
    TEST_ASSERT_EQUAL_STRING("a", refs[3]);
    TEST_ASSERT_FALSE(ui_devl_favorite(&s_l, &devs[3]));
    TEST_ASSERT_EQUAL_size_t(3, page_refs(devs, n, "Hall", refs)); /* Hall untouched */
    TEST_ASSERT_EQUAL_STRING("e", refs[0]);
    TEST_ASSERT_EQUAL_STRING("c", refs[1]);
    TEST_ASSERT_EQUAL_STRING("a", refs[2]);
    /* stored and read back as moved */
    const size_t len = ui_devl_encode(&s_l, s_blob, sizeof(s_blob));
    TEST_ASSERT_TRUE(len > 0u);
    TEST_ASSERT_TRUE(ui_devl_decode(&s_l2, s_blob, len));
    TEST_ASSERT_EQUAL_UINT8(s_l.n, s_l2.n);
    for (size_t i = 0; i < s_l.n; i++) {
        TEST_ASSERT_EQUAL_STRING(s_l.e[i].ref, s_l2.e[i].ref);
        TEST_ASSERT_EQUAL_HEX8(s_l.e[i].flags, s_l2.e[i].flags);
    }
    /* an area's page the layout doesn't name: its devices are named, then moved */
    ui_devl_clear(&s_l);
    TEST_ASSERT_TRUE(ui_devl_name(&s_l, "a", UI_DEVL_FAV));
    TEST_ASSERT_TRUE(ui_devl_move_to(&s_l, devs, n, "Hall", "e", 0));
    TEST_ASSERT_EQUAL_size_t(3, page_refs(devs, n, "Hall", refs));
    TEST_ASSERT_EQUAL_STRING("e", refs[0]);
    TEST_ASSERT_EQUAL_STRING("a", refs[1]);
    TEST_ASSERT_EQUAL_STRING("c", refs[2]);
    TEST_ASSERT_EQUAL_size_t(1, page_refs(devs, n, NULL, refs)); /* Favorites still only a */
    TEST_ASSERT_EQUAL_STRING("a", refs[0]);
    /* a full layout of devices still there: the page can't be named, nothing moves */
    ui_devl_clear(&s_l);
    for (unsigned i = 0; i <= UI_DEVL_MAX; i++) {
        char ref[24];
        snprintf(ref, sizeof(ref), "switch.s%u", i);
        s_many[i] = dev(ref, DEV_SWITCH, 0, i == UI_DEVL_MAX || i == 0u ? "Attic" : "Hall");
        if (i < UI_DEVL_MAX) {
            TEST_ASSERT_TRUE(ui_devl_name(&s_l, ref, UI_DEVL_FAV));
        }
    }
    char last[24];
    snprintf(last, sizeof(last), "switch.s%u", (unsigned)UI_DEVL_MAX);
    TEST_ASSERT_FALSE(ui_devl_move_to(&s_l, s_many, UI_DEVL_MAX + 1u, "Attic", last, 0));
    TEST_ASSERT_EQUAL_UINT8(UI_DEVL_MAX, s_l.n);
}

/* HCTL-006: the source's words, read for the kind they belong to. */
static void test_state_words(void)
{
    TEST_ASSERT_EQUAL_UINT8(UI_DEV_ST_ON, ui_dev_state_parse(DEV_LIGHT, "on"));
    TEST_ASSERT_EQUAL_UINT8(UI_DEV_ST_OFF, ui_dev_state_parse(DEV_SWITCH, "off"));
    TEST_ASSERT_EQUAL_UINT8(UI_DEV_ST_LOCKED, ui_dev_state_parse(DEV_LOCK, "locked"));
    TEST_ASSERT_EQUAL_UINT8(UI_DEV_ST_JAMMED, ui_dev_state_parse(DEV_LOCK, "jammed"));
    TEST_ASSERT_EQUAL_UINT8(UI_DEV_ST_UNLOCKING, ui_dev_state_parse(DEV_LOCK, "unlocking"));
    TEST_ASSERT_EQUAL_UINT8(UI_DEV_ST_CLOSING, ui_dev_state_parse(DEV_COVER, "closing"));
    TEST_ASSERT_EQUAL_UINT8(UI_DEV_ST_OPEN, ui_dev_state_parse(DEV_VALVE, "open"));
    TEST_ASSERT_EQUAL_UINT8(UI_DEV_ST_AUTO, ui_dev_state_parse(DEV_CLIMATE, "heat_cool"));
    TEST_ASSERT_EQUAL_UINT8(UI_DEV_ST_HEAT, ui_dev_state_parse(DEV_CLIMATE, "heat"));
    TEST_ASSERT_EQUAL_UINT8(UI_DEV_ST_UNKNOWN, ui_dev_state_parse(DEV_LIGHT, "unavailable"));
    TEST_ASSERT_EQUAL_UINT8(UI_DEV_ST_UNKNOWN, ui_dev_state_parse(DEV_LOCK, "on"));
    TEST_ASSERT_EQUAL_UINT8(UI_DEV_ST_UNKNOWN, ui_dev_state_parse(DEV_SCENE, "2026-10-01T23:05:00"));
    TEST_ASSERT_EQUAL_UINT8(UI_DEV_ST_UNKNOWN, ui_dev_state_parse(DEV_LIGHT, NULL));
}

/* HCTL-002 / HCTL-004 (BL-171): a device's values become the screen's fields the sheets read -
 * nothing known reads as before (no slider, no mode keys, "Heat to -"), heat_cool is the Auto key,
 * an out-of-range level is not known, and the caps the port already set stay. */
static void test_values_fill(void)
{
    ui_dev_t u;
    dev_values_t v;
    memset(&u, 0, sizeof(u));
    memset(&v, 0, sizeof(v));
    u.caps = UI_DEV_CAP_ENTRANCE;
    ui_dev_values_fill(&u, &v);
    TEST_ASSERT_EQUAL_INT8(-1, u.level);
    TEST_ASSERT_EQUAL_HEX8(0, u.modes);
    TEST_ASSERT_EQUAL_HEX8(UI_DEV_CAP_ENTRANCE, u.caps);
    TEST_ASSERT_EQUAL_INT32(UI_DEV_TEMP_UNKNOWN, u.setpoint);
    TEST_ASSERT_EQUAL_INT32(UI_DEV_TEMP_UNKNOWN, u.temperature);

    v.flags = DEV_VAL_LEVEL | DEV_VAL_LEVEL_SET | DEV_VAL_SETPOINT | DEV_VAL_TEMP;
    v.level = 100;
    v.setpoint = -250;
    v.temp = 7150;
    v.modes = DEV_MODE_OFF | DEV_MODE_HEAT | DEV_MODE_COOL | DEV_MODE_HEAT_COOL;
    ui_dev_values_fill(&u, &v);
    TEST_ASSERT_EQUAL_INT8(100, u.level);
    TEST_ASSERT_EQUAL_HEX8(UI_DEV_CAP_ENTRANCE | UI_DEV_CAP_LEVEL, u.caps);
    TEST_ASSERT_EQUAL_HEX8(UI_DEV_MODE_OFF | UI_DEV_MODE_HEAT | UI_DEV_MODE_COOL | UI_DEV_MODE_AUTO,
                           u.modes);
    TEST_ASSERT_EQUAL_INT32(-250, u.setpoint);
    TEST_ASSERT_EQUAL_INT32(7150, u.temperature);

    /* "auto" alone is Auto too; a dimmable light that is off has a slider and no level */
    memset(&u, 0, sizeof(u));
    v.flags = DEV_VAL_LEVEL_SET;
    v.modes = DEV_MODE_AUTO;
    ui_dev_values_fill(&u, &v);
    TEST_ASSERT_EQUAL_INT8(-1, u.level);
    TEST_ASSERT_EQUAL_HEX8(UI_DEV_CAP_LEVEL, u.caps);
    TEST_ASSERT_EQUAL_HEX8(UI_DEV_MODE_AUTO, u.modes);

    /* a level past 100 is never shown */
    v.flags = DEV_VAL_LEVEL;
    v.level = 200;
    ui_dev_values_fill(&u, &v);
    TEST_ASSERT_EQUAL_INT8(-1, u.level);
}

static uint8_t caps_filled(uint8_t kind, uint8_t dev_caps)
{
    ui_dev_t u;
    dev_values_t v;
    memset(&u, 0, sizeof(u));
    memset(&v, 0, sizeof(v));
    u.kind = kind;
    v.caps = dev_caps;
    ui_dev_values_fill(&u, &v);
    return u.caps;
}

/* HCTL-002, HCTL-004 (BL-171, BL-174): what the source says a device can't do becomes the
 * screen's negative bits - a cover that can't stop, a fan without speeds; not known (caps 0, an
 * older integration) keeps every key; other kinds never get them. since becomes since_ms. */
static void test_values_fill_caps(void)
{
    TEST_ASSERT_EQUAL_HEX8(0, caps_filled(DEV_COVER, 0u));
    TEST_ASSERT_EQUAL_HEX8(UI_DEV_CAP_NO_STOP, caps_filled(DEV_COVER, DEV_CAP_KNOWN));
    TEST_ASSERT_EQUAL_HEX8(UI_DEV_CAP_NO_STOP,
                           caps_filled(DEV_COVER, DEV_CAP_KNOWN | DEV_CAP_POSITION));
    TEST_ASSERT_EQUAL_HEX8(0, caps_filled(DEV_COVER, DEV_CAP_KNOWN | DEV_CAP_STOP));
    TEST_ASSERT_EQUAL_HEX8(UI_DEV_CAP_NO_SPEED, caps_filled(DEV_FAN, DEV_CAP_KNOWN));
    TEST_ASSERT_EQUAL_HEX8(0, caps_filled(DEV_FAN, DEV_CAP_KNOWN | DEV_CAP_SPEED));
    TEST_ASSERT_EQUAL_HEX8(0, caps_filled(DEV_FAN, 0u));
    TEST_ASSERT_EQUAL_HEX8(0, caps_filled(DEV_LIGHT, DEV_CAP_KNOWN));
    TEST_ASSERT_EQUAL_HEX8(0, caps_filled(DEV_SWITCH, DEV_CAP_KNOWN));
    TEST_ASSERT_EQUAL_HEX8(0, caps_filled(DEV_VALVE, DEV_CAP_KNOWN));

    ui_dev_t u;
    dev_values_t v;
    memset(&u, 0, sizeof(u));
    memset(&v, 0, sizeof(v));
    u.kind = DEV_SCENE;
    v.since = 1791320712u;
    ui_dev_values_fill(&u, &v);
    TEST_ASSERT_TRUE(u.since_ms == 1791320712000LL);
    v.since = 4294967295u; /* the last second the wire can say: no overflow */
    ui_dev_values_fill(&u, &v);
    TEST_ASSERT_TRUE(u.since_ms == 4294967295000LL);
    v.since = 0u;
    ui_dev_values_fill(&u, &v);
    TEST_ASSERT_TRUE(u.since_ms == 0);
}

/* HCTL-004 (BL-171): what a thermostat is doing reaches the screen as its source said it; a number
 * past the enum is not known, and no other kind carries one. The fill leaves `unsure` (the port's)
 * alone. */
static void test_values_fill_hvac_action(void)
{
    ui_dev_t u;
    dev_values_t v;
    memset(&u, 0, sizeof(u));
    memset(&v, 0, sizeof(v));
    u.kind = DEV_CLIMATE;
    u.unsure = 1u;
    for (unsigned a = 0u; a < (unsigned)DEV_HVAC_COUNT; a++) {
        v.hvac_action = (uint8_t)a;
        u.hvac_action = 0xA5u;
        ui_dev_values_fill(&u, &v);
        TEST_ASSERT_EQUAL_UINT8(a, u.hvac_action);
    }
    TEST_ASSERT_EQUAL_UINT8(DEV_HVAC_DEFROSTING, DEV_HVAC_COUNT - 1); /* 0..8, as the wire's */
    v.hvac_action = (uint8_t)DEV_HVAC_COUNT; /* 9: a newer source's word this firmware lacks */
    ui_dev_values_fill(&u, &v);
    TEST_ASSERT_EQUAL_UINT8(DEV_HVAC_UNKNOWN, u.hvac_action);
    v.hvac_action = 255u;
    ui_dev_values_fill(&u, &v);
    TEST_ASSERT_EQUAL_UINT8(DEV_HVAC_UNKNOWN, u.hvac_action);
    TEST_ASSERT_EQUAL_UINT8(1u, u.unsure);

    memset(&u, 0, sizeof(u));
    u.kind = DEV_LIGHT;
    u.hvac_action = 0xA5u;
    v.hvac_action = (uint8_t)DEV_HVAC_HEATING;
    ui_dev_values_fill(&u, &v);
    TEST_ASSERT_EQUAL_UINT8(DEV_HVAC_UNKNOWN, u.hvac_action);
}

/* HCTL-004: a fan is wide by its kind, but one without speeds is one cell, as a switch is; a
 * size the person stored still wins. */
static void test_kind_wide_fan_without_speeds(void)
{
    ui_dev_t devs[] = {
        dev("fan.porch", DEV_FAN, 0, "Porch"),
        dev("fan.ceiling", DEV_FAN, UI_DEV_CAP_NO_SPEED, "Porch"),
    };
    const size_t n = sizeof(devs) / sizeof(devs[0]);
    TEST_ASSERT_TRUE(ui_devl_kind_wide(&devs[0]));
    TEST_ASSERT_FALSE(ui_devl_kind_wide(&devs[1]));
    ui_devl_clear(&s_l);
    TEST_ASSERT_TRUE(ui_devl_wide(&s_l, &devs[0]));
    TEST_ASSERT_FALSE(ui_devl_wide(&s_l, &devs[1]));
    TEST_ASSERT_TRUE(ui_devl_add(&s_l, devs, n, "fan.ceiling"));
    TEST_ASSERT_TRUE(ui_devl_set_size(&s_l, devs, n, "fan.ceiling", UI_DEVL_SIZE_WIDE));
    TEST_ASSERT_TRUE(ui_devl_wide(&s_l, &devs[1]));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fixture_v1_reads);
    RUN_TEST(test_round_trip_at_the_bound);
    RUN_TEST(test_garbage_is_refused);
    RUN_TEST(test_reserved_bits_and_duplicates);
    RUN_TEST(test_defaults);
    RUN_TEST(test_stored_layout);
    RUN_TEST(test_arrange_edits);
    RUN_TEST(test_arrange_move_to);
    RUN_TEST(test_state_words);
    RUN_TEST(test_values_fill);
    RUN_TEST(test_values_fill_caps);
    RUN_TEST(test_values_fill_hvac_action);
    RUN_TEST(test_kind_wide_fan_without_speeds);
    return UNITY_END();
}
