/* UI-046 (docs/features/07), LINK-051 and LINK-052 (docs/features/11 "Settings fields", "Applying
 * a change from elsewhere"): a change of the Screen and sound settings from elsewhere, as the
 * screen merges it and as the mailbox between the link (or Home Assistant) and the screen's task
 * keeps it. The ids, the look's fields, the why numbers and every range are alarm_proto's
 * (proto_prefs.h); the fields the screen holds for 2 s are every one that moves its light or
 * palette (ui_light.h), and the clock's format; a merge writes only the fields a patch names,
 * never the partition, the language or the region; the mailbox holds one patch, withdraws one not
 * taken within 3 s and answers each ticket once. */
#include "ui_backend/ui_backend.h"
#include "ui_backend/ui_light.h"
#include "ui_backend/ui_prefs.h"

#include "alarm_proto/proto_prefs.h"
#include "unity.h"

#include <string.h>

void setUp(void);
void tearDown(void);
void setUp(void) {}
void tearDown(void) {}

/* The UI's copy of the frozen numbers (ui_prefs.h) is the wire's (proto_prefs.h). */
static void test_numbers_are_the_wires(void)
{
    TEST_ASSERT_EQUAL_UINT(PROTO_PREFS_F_LAST, UI_PREFS_F_LAST);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_F_BRIGHTNESS, UI_PREFS_F_BRIGHTNESS);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_F_SAVER_BRIGHTNESS, UI_PREFS_F_SAVER_BRIGHTNESS);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_F_SAVER_AFTER, UI_PREFS_F_SAVER_AFTER);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_F_OFF_AFTER, UI_PREFS_F_OFF_AFTER);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_F_NIGHT_ON, UI_PREFS_F_NIGHT_ON);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_F_NIGHT_START, UI_PREFS_F_NIGHT_START);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_F_NIGHT_END, UI_PREFS_F_NIGHT_END);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_F_NIGHT_BRIGHTNESS, UI_PREFS_F_NIGHT_BRIGHTNESS);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_F_NIGHT_OFF_AFTER, UI_PREFS_F_NIGHT_OFF_AFTER);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_F_CLOCK_MODE, UI_PREFS_F_CLOCK_MODE);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_F_APPEARANCE, UI_PREFS_F_APPEARANCE);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_F_HIGH_CONTRAST, UI_PREFS_F_HIGH_CONTRAST);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_F_VOLUME, UI_PREFS_F_VOLUME);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_F_CHIME_VOLUME, UI_PREFS_F_CHIME_VOLUME);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_F_VOICE_VOLUME, UI_PREFS_F_VOICE_VOLUME);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_F_KEY_CLICK, UI_PREFS_F_KEY_CLICK);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_F_VOICE_ENABLED, UI_PREFS_F_VOICE_ENABLED);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_F_VOICE_STATUS, UI_PREFS_F_VOICE_STATUS);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_F_VOICE_CHIME, UI_PREFS_F_VOICE_CHIME);
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_KNOWN, UI_PREFS_KNOWN);
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_LOOK, UI_PREFS_LOOK);
    for (unsigned id = 1u; id <= UI_PREFS_F_LAST; id++) {
        TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_BIT(id), UI_PREFS_BIT(id));
        /* UI-046: the look's fields are the table's LOOK fields */
        const proto_prefs_field_t *f = proto_prefs_field(id);
        TEST_ASSERT_NOT_NULL(f);
        TEST_ASSERT_EQUAL((f->flags & PROTO_PREFS_FL_LOOK) != 0u, (UI_PREFS_LOOK & UI_PREFS_BIT(id)) != 0u);
    }
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_NONE, UI_PREFS_WHY_NONE);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_NOT_ALLOWED, UI_PREFS_WHY_NOT_ALLOWED);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_RANGE, UI_PREFS_WHY_RANGE);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_IN_USE, UI_PREFS_WHY_IN_USE);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_HELD, UI_PREFS_WHY_HELD);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_VOICE_NOT_READY, UI_PREFS_WHY_VOICE_NOT_READY);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_BAD, UI_PREFS_WHY_BAD);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_BUSY, UI_PREFS_WHY_BUSY);
}

/* x and y light the screen differently or give it another palette (ui_light.h) at some idle time,
 * minute or alarm. */
static bool light_differs(const ui_options_t *x, const ui_options_t *y)
{
    static const uint32_t idles[] = {0u, 5000u, 10000u, 30000u, 60000u, 120000u, 900000u};
    static const int minutes[] = {-1, 0, 300, 720, 1330};
    for (size_t m = 0; m < sizeof(minutes) / sizeof(minutes[0]); m++) {
        if (ui_light_theme(x, minutes[m]) != ui_light_theme(y, minutes[m])) {
            return true;
        }
        const bool nx = ui_light_night(x, minutes[m]);
        const bool ny = ui_light_night(y, minutes[m]);
        for (size_t i = 0; i < sizeof(idles) / sizeof(idles[0]); i++) {
            const ui_light_stage_t sx = ui_light_stage(x, idles[i], nx);
            const ui_light_stage_t sy = ui_light_stage(y, idles[i], ny);
            for (unsigned alarm = 0u; alarm < 2u; alarm++) {
                if (sx != sy || ui_light_percent(x, sx, nx, alarm != 0u) !=
                                    ui_light_percent(y, sy, ny, alarm != 0u)) {
                    return true;
                }
            }
        }
    }
    return false;
}

/* UI-046, UI-020 (the backlight never flickers): the fields the screen holds for UI_PREFS_LOOK_MS
 * (UI_PREFS_SHOWN) are exactly those whose change can move the backlight or the palette at once -
 * whatever ui_light.h reads, from the defaults and from a night with the screen going dark - plus
 * the clock's format, whose words change. So the look's fields and the three delays that take a
 * screen nobody touches from the saver to dark and back (ids 3, 4, 9); never a sound's. */
static void test_shown_is_what_moves_the_screen(void)
{
    static const uint32_t values[] = {0u,   1u,    2u,    10u,    25u,    50u,    60u,
                                      100u, 360u, 1320u, 1439u, 10000u, 60000u, 600000u};
    uint32_t moves = 0u;
    for (unsigned base = 0u; base < 2u; base++) {
        ui_options_t o;
        ui_options_defaults(&o);
        if (base == 1u) {
            o.night_on = true; /* 22:00 to 07:00 */
            o.off_after_ms = 600000u;
            o.appearance = UI_APPEARANCE_AUTO;
        }
        for (unsigned id = 1u; id <= UI_PREFS_F_LAST; id++) {
            for (size_t a = 0; a < sizeof(values) / sizeof(values[0]); a++) {
                for (size_t b = 0; b < sizeof(values) / sizeof(values[0]); b++) {
                    if (a == b || !ui_prefs_valid(id, values[a]) || !ui_prefs_valid(id, values[b])) {
                        continue;
                    }
                    ui_options_t x = o;
                    ui_options_t y = o;
                    ui_prefs_store(&x, id, values[a]);
                    ui_prefs_store(&y, id, values[b]);
                    if (light_differs(&x, &y)) {
                        moves |= UI_PREFS_BIT(id);
                    }
                }
            }
        }
    }
    const uint32_t clock_bit = UI_PREFS_BIT(UI_PREFS_F_CLOCK_MODE);
    const uint32_t delays = UI_PREFS_BIT(UI_PREFS_F_SAVER_AFTER) | UI_PREFS_BIT(UI_PREFS_F_OFF_AFTER) |
                            UI_PREFS_BIT(UI_PREFS_F_NIGHT_OFF_AFTER);
    TEST_ASSERT_EQUAL_HEX32(UI_PREFS_SHOWN & ~clock_bit, moves);
    TEST_ASSERT_EQUAL_HEX32(delays, moves & delays);
    TEST_ASSERT_EQUAL_HEX32(UI_PREFS_LOOK | delays | clock_bit, UI_PREFS_SHOWN);
}

/* Every field takes exactly what the wire's table takes: the edges of each range, around them,
 * and a sweep. An id this firmware doesn't know takes nothing. */
static void test_ranges_are_the_wires(void)
{
    static const uint32_t edges[] = {9999u,     10000u,     10001u,       86399999u, 86400000u,
                                     86400001u, 1438u,      1439u,        1440u,       UINT32_MAX,
                                     0x80000000u, 65535u,   65536u,       256u,        255u};
    for (unsigned id = 0u; id <= UI_PREFS_F_LAST + 2u; id++) {
        for (uint32_t v = 0u; v <= 300u; v++) {
            TEST_ASSERT_EQUAL_MESSAGE(proto_prefs_check(id, v) == PROTO_PREFS_WHY_NONE,
                                      ui_prefs_valid(id, v), "a value the wire takes");
        }
        for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
            TEST_ASSERT_EQUAL_MESSAGE(proto_prefs_check(id, edges[i]) == PROTO_PREFS_WHY_NONE,
                                      ui_prefs_valid(id, edges[i]), "a range's edge");
        }
        for (uint32_t v = 9000u; v <= 11000u; v++) {
            TEST_ASSERT_EQUAL(proto_prefs_check(id, v) == PROTO_PREFS_WHY_NONE, ui_prefs_valid(id, v));
        }
    }
    TEST_ASSERT_FALSE(ui_prefs_valid(0u, 0u));
    TEST_ASSERT_FALSE(ui_prefs_valid(UI_PREFS_F_LAST + 1u, 0u));
}

/* The screen's defaults (ui_options_defaults) as fields are the panel's defaults as fields
 * (panel_prefs_defaults; alarm_proto's test_prefs.c GOLDEN_CRC): the two copies read the same. */
static void test_defaults_read_as_the_panels(void)
{
    ui_options_t opt;
    ui_options_defaults(&opt);
    proto_prefs_values_t v;
    memset(&v, 0, sizeof v);
    for (unsigned id = 1u; id <= UI_PREFS_F_LAST; id++) {
        uint32_t x = 0xFFFFFFFFu;
        TEST_ASSERT_TRUE(ui_prefs_get(&opt, id, &x));
        TEST_ASSERT_TRUE(ui_prefs_valid(id, x));
        TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_NONE, proto_prefs_put(&v, id, x));
    }
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_KNOWN, v.have);
    TEST_ASSERT_EQUAL_HEX32(0x31DE243Eu, proto_prefs_values_crc(&v));
    uint32_t x = 7u;
    TEST_ASSERT_FALSE(ui_prefs_get(&opt, 0u, &x));
    TEST_ASSERT_FALSE(ui_prefs_get(&opt, UI_PREFS_F_LAST + 1u, &x));
    TEST_ASSERT_EQUAL_UINT32(7u, x);
}

/* Everything a merge must never touch, and every field. */
static void assert_same_but(const ui_options_t *a, const ui_options_t *b, uint32_t except)
{
    TEST_ASSERT_EQUAL_UINT8(a->partition, b->partition);
    TEST_ASSERT_EQUAL_STRING(a->lang, b->lang);
    TEST_ASSERT_EQUAL_STRING(a->region, b->region);
    TEST_ASSERT_EQUAL(a->run_setup, b->run_setup);
    for (unsigned id = 1u; id <= UI_PREFS_F_LAST; id++) {
        if ((except & UI_PREFS_BIT(id)) != 0u) {
            continue;
        }
        uint32_t x = 0u;
        uint32_t y = 1u;
        TEST_ASSERT_TRUE(ui_prefs_get(a, id, &x));
        TEST_ASSERT_TRUE(ui_prefs_get(b, id, &y));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(x, y, "a field the patch didn't name");
    }
}

static ui_options_t unusual(void)
{
    ui_options_t o;
    ui_options_defaults(&o);
    o.partition = 3u;
    memcpy(o.lang, "de-DE", 6u);
    memcpy(o.region, "AT", 3u);
    o.run_setup = true;
    o.brightness = 55u;
    o.night_on = true;
    o.appearance = 2u;
    o.voice_status = false;
    o.chime_volume = 12u;
    return o;
}

/* UI-046: the fields named, and nothing else: the partition, the language and the region stay
 * (LINK-052). Only a value that differs counts as a change. */
static void test_merge_takes_only_named_fields(void)
{
    const ui_options_t before = unusual();
    ui_options_t opt = before;
    ui_prefs_patch_t p;
    memset(&p, 0, sizeof p);
    TEST_ASSERT_TRUE(ui_prefs_put(&p, UI_PREFS_F_BRIGHTNESS, 40u));
    TEST_ASSERT_TRUE(ui_prefs_put(&p, UI_PREFS_F_APPEARANCE, 1u));
    TEST_ASSERT_TRUE(ui_prefs_put(&p, UI_PREFS_F_NIGHT_ON, 1u)); /* already on */
    TEST_ASSERT_TRUE(ui_prefs_put(&p, UI_PREFS_F_VOLUME, 0u));   /* decision 6: Beeps may be 0 */
    uint8_t why[UI_PREFS_F_LAST];
    memset(why, 0xEE, sizeof why);
    const uint32_t changed = ui_prefs_merge(&opt, &p, why);
    TEST_ASSERT_EQUAL_HEX32(UI_PREFS_BIT(UI_PREFS_F_BRIGHTNESS) | UI_PREFS_BIT(UI_PREFS_F_APPEARANCE) |
                                UI_PREFS_BIT(UI_PREFS_F_VOLUME),
                            changed);
    TEST_ASSERT_EQUAL_UINT8(40u, opt.brightness);
    TEST_ASSERT_EQUAL_UINT8(UI_APPEARANCE_LIGHT, opt.appearance);
    TEST_ASSERT_TRUE(opt.night_on);
    TEST_ASSERT_EQUAL_UINT8(0u, opt.volume);
    assert_same_but(&before, &opt, changed);
    for (size_t i = 0; i < sizeof why; i++) {
        TEST_ASSERT_EQUAL_HEX8(0xEEu, why[i]); /* nothing refused: why left alone */
    }
    /* the same patch again changes nothing */
    TEST_ASSERT_EQUAL_HEX32(0u, ui_prefs_merge(&opt, &p, why));
    /* an empty patch changes nothing */
    ui_prefs_patch_t none;
    memset(&none, 0, sizeof none);
    const ui_options_t now = opt;
    TEST_ASSERT_EQUAL_HEX32(0u, ui_prefs_merge(&opt, &none, why));
    assert_same_but(&now, &opt, 0u);
}

/* Each field alone, at both ends of its range: written, read back, nothing else moved. */
static void test_merge_every_field(void)
{
    for (unsigned id = 1u; id <= UI_PREFS_F_LAST; id++) {
        const proto_prefs_field_t *f = proto_prefs_field(id);
        TEST_ASSERT_NOT_NULL(f);
        const uint32_t ends[3] = {f->min, f->max, (f->flags & PROTO_PREFS_FL_DELAY) != 0u ? 0u : f->min};
        for (size_t e = 0; e < 3u; e++) {
            const ui_options_t before = unusual();
            ui_options_t opt = before;
            ui_prefs_patch_t p;
            memset(&p, 0, sizeof p);
            TEST_ASSERT_TRUE(ui_prefs_put(&p, id, ends[e]));
            uint8_t why[UI_PREFS_F_LAST];
            memset(why, 0, sizeof why);
            const uint32_t changed = ui_prefs_merge(&opt, &p, why);
            uint32_t got = 0xFFFFFFFFu;
            TEST_ASSERT_TRUE(ui_prefs_get(&opt, id, &got));
            TEST_ASSERT_EQUAL_UINT32(ends[e], got);
            uint32_t was = 0u;
            TEST_ASSERT_TRUE(ui_prefs_get(&before, id, &was));
            TEST_ASSERT_EQUAL_HEX32(was != ends[e] ? UI_PREFS_BIT(id) : 0u, changed);
            assert_same_but(&before, &opt, UI_PREFS_BIT(id));
            for (size_t i = 0; i < sizeof why; i++) {
                TEST_ASSERT_EQUAL_HEX8(0u, why[i]);
            }
        }
    }
}

/* A value outside its range is refused for that field alone (RANGE), the field as it was; the
 * rest of the patch is merged. A bit past the last id is no field and is ignored. */
static void test_merge_refuses_a_range_alone(void)
{
    const ui_options_t before = unusual();
    ui_options_t opt = before;
    ui_prefs_patch_t p;
    memset(&p, 0, sizeof p);
    ui_prefs_put(&p, UI_PREFS_F_BRIGHTNESS, 5u);
    ui_prefs_put(&p, UI_PREFS_F_SAVER_AFTER, 5000u);
    ui_prefs_put(&p, UI_PREFS_F_NIGHT_START, 1440u);
    ui_prefs_put(&p, UI_PREFS_F_CLOCK_MODE, 3u);
    ui_prefs_put(&p, UI_PREFS_F_VOICE_ENABLED, 2u);
    ui_prefs_put(&p, UI_PREFS_F_KEY_CLICK, 0u);
    ui_prefs_put(&p, UI_PREFS_F_OFF_AFTER, 0u); /* never: already */
    ui_prefs_put(&p, UI_PREFS_F_NIGHT_OFF_AFTER, 86400000u);
    p.have |= 0xFFF80000u; /* ids this firmware doesn't know */
    TEST_ASSERT_FALSE(ui_prefs_put(&p, 0u, 1u));
    TEST_ASSERT_FALSE(ui_prefs_put(&p, UI_PREFS_F_LAST + 1u, 1u));
    uint8_t why[UI_PREFS_F_LAST];
    memset(why, 0, sizeof why);
    const uint32_t changed = ui_prefs_merge(&opt, &p, why);
    TEST_ASSERT_EQUAL_HEX32(UI_PREFS_BIT(UI_PREFS_F_KEY_CLICK) | UI_PREFS_BIT(UI_PREFS_F_NIGHT_OFF_AFTER),
                            changed);
    TEST_ASSERT_FALSE(opt.key_click);
    TEST_ASSERT_EQUAL_UINT32(86400000u, opt.night_off_after_ms);
    assert_same_but(&before, &opt, changed);
    for (unsigned id = 1u; id <= UI_PREFS_F_LAST; id++) {
        const bool range = id == UI_PREFS_F_BRIGHTNESS || id == UI_PREFS_F_SAVER_AFTER ||
                           id == UI_PREFS_F_NIGHT_START || id == UI_PREFS_F_CLOCK_MODE ||
                           id == UI_PREFS_F_VOICE_ENABLED;
        TEST_ASSERT_EQUAL_UINT8(range ? UI_PREFS_WHY_RANGE : UI_PREFS_WHY_NONE, why[id - 1u]);
    }
}

static void test_refuse_all(void)
{
    ui_prefs_patch_t p;
    memset(&p, 0, sizeof p);
    ui_prefs_put(&p, UI_PREFS_F_BRIGHTNESS, 40u);
    ui_prefs_put(&p, UI_PREFS_F_VOICE_CHIME, 0u);
    p.have |= 0x80000000u;
    uint8_t why[UI_PREFS_F_LAST];
    memset(why, 0, sizeof why);
    ui_prefs_refuse_all(&p, UI_PREFS_WHY_HELD, why);
    for (unsigned id = 1u; id <= UI_PREFS_F_LAST; id++) {
        const bool named = id == UI_PREFS_F_BRIGHTNESS || id == UI_PREFS_F_VOICE_CHIME;
        TEST_ASSERT_EQUAL_UINT8(named ? UI_PREFS_WHY_HELD : 0u, why[id - 1u]);
    }
}

/* LINK-051: one change at a time: submitted, taken, answered once; another is BUSY at once while
 * one is in the box, waiting or taken. */
static void test_box_one_at_a_time(void)
{
    ui_prefs_box_t b;
    memset(&b, 0, sizeof b);
    uint32_t t = 0u;
    TEST_ASSERT_EQUAL_INT(UI_PREFS_TAKE_NONE, ui_prefs_box_take(&b, 100u, &t));
    TEST_ASSERT_EQUAL_UINT32(0u, t);
    const uint32_t a = ui_prefs_box_submit(&b, 100u);
    TEST_ASSERT_NOT_EQUAL(0u, a);
    TEST_ASSERT_EQUAL_UINT32(0u, ui_prefs_box_submit(&b, 150u)); /* waiting: busy */
    TEST_ASSERT_FALSE(ui_prefs_box_done(&b, a));                 /* not taken: no answer yet */
    TEST_ASSERT_EQUAL_INT(UI_PREFS_TAKE_OK, ui_prefs_box_take(&b, 200u, &t));
    TEST_ASSERT_EQUAL_UINT32(a, t);
    TEST_ASSERT_EQUAL_INT(UI_PREFS_TAKE_NONE, ui_prefs_box_take(&b, 210u, &t)); /* taken once */
    TEST_ASSERT_EQUAL_UINT32(0u, ui_prefs_box_submit(&b, 220u)); /* taken: busy */
    TEST_ASSERT_FALSE(ui_prefs_box_cancel(&b, a)); /* the screen has it: its answer follows */
    TEST_ASSERT_FALSE(ui_prefs_box_done(&b, a + 1u)); /* another ticket's answer: dropped */
    TEST_ASSERT_TRUE(ui_prefs_box_done(&b, a));
    TEST_ASSERT_FALSE(ui_prefs_box_done(&b, a)); /* answered once */
    TEST_ASSERT_FALSE(ui_prefs_box_done(&b, 0u));
    const uint32_t c = ui_prefs_box_submit(&b, 5000u);
    TEST_ASSERT_NOT_EQUAL(0u, c);
    TEST_ASSERT_NOT_EQUAL(a, c);
}

/* docs/features/11: a patch not taken within 3 s is withdrawn and answered BUSY - never applied
 * late. At 2999 ms it is still taken; at 3000 ms the screen's look withdraws it. */
static void test_box_expires_after_3_s(void)
{
    ui_prefs_box_t b;
    memset(&b, 0, sizeof b);
    uint32_t t = 0u;
    uint32_t a = ui_prefs_box_submit(&b, 1000u);
    TEST_ASSERT_EQUAL_INT(UI_PREFS_TAKE_OK, ui_prefs_box_take(&b, 1000u + UI_PREFS_TAKE_MS - 1u, &t));
    TEST_ASSERT_TRUE(ui_prefs_box_done(&b, a));
    a = ui_prefs_box_submit(&b, 2000u);
    TEST_ASSERT_EQUAL_INT(UI_PREFS_TAKE_EXPIRED, ui_prefs_box_take(&b, 2000u + UI_PREFS_TAKE_MS, &t));
    TEST_ASSERT_EQUAL_UINT32(a, t);
    TEST_ASSERT_FALSE(ui_prefs_box_done(&b, a)); /* withdrawn: the screen never had it */
    TEST_ASSERT_FALSE(ui_prefs_box_cancel(&b, a));
    TEST_ASSERT_EQUAL_INT(UI_PREFS_TAKE_NONE, ui_prefs_box_take(&b, 6000u, &t));
    /* the box is free for the next one */
    TEST_ASSERT_NOT_EQUAL(0u, ui_prefs_box_submit(&b, 6000u));
    /* the clock wraps between the submit and the look: still 3 s */
    memset(&b, 0, sizeof b);
    a = ui_prefs_box_submit(&b, UINT32_MAX - 1000u);
    TEST_ASSERT_EQUAL_INT(UI_PREFS_TAKE_OK, ui_prefs_box_take(&b, 1998u, &t));
    TEST_ASSERT_TRUE(ui_prefs_box_done(&b, a));
    a = ui_prefs_box_submit(&b, UINT32_MAX - 1000u);
    TEST_ASSERT_EQUAL_INT(UI_PREFS_TAKE_EXPIRED, ui_prefs_box_take(&b, 1999u, &t));
}

/* The submitter's own give-up (the screen's task never looked): withdrawn while waiting, so no
 * answer will come and the submitter answers BUSY itself; a stale ticket withdraws nothing. */
static void test_box_cancel(void)
{
    ui_prefs_box_t b;
    memset(&b, 0, sizeof b);
    uint32_t t = 0u;
    const uint32_t a = ui_prefs_box_submit(&b, 0u);
    TEST_ASSERT_FALSE(ui_prefs_box_cancel(&b, a + 1u));
    TEST_ASSERT_FALSE(ui_prefs_box_cancel(&b, 0u));
    TEST_ASSERT_TRUE(ui_prefs_box_cancel(&b, a));
    TEST_ASSERT_FALSE(ui_prefs_box_cancel(&b, a));
    TEST_ASSERT_EQUAL_INT(UI_PREFS_TAKE_NONE, ui_prefs_box_take(&b, 10u, &t));
    TEST_ASSERT_FALSE(ui_prefs_box_done(&b, a));
}

/* A ticket is never 0, also when the count wraps. */
static void test_box_ticket_never_0(void)
{
    ui_prefs_box_t b;
    memset(&b, 0, sizeof b);
    b.last = UINT32_MAX - 1u;
    uint32_t a = ui_prefs_box_submit(&b, 0u);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, a);
    TEST_ASSERT_TRUE(ui_prefs_box_cancel(&b, a));
    a = ui_prefs_box_submit(&b, 0u);
    TEST_ASSERT_EQUAL_UINT32(1u, a);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_numbers_are_the_wires);
    RUN_TEST(test_ranges_are_the_wires);
    RUN_TEST(test_shown_is_what_moves_the_screen);
    RUN_TEST(test_defaults_read_as_the_panels);
    RUN_TEST(test_merge_takes_only_named_fields);
    RUN_TEST(test_merge_every_field);
    RUN_TEST(test_merge_refuses_a_range_alone);
    RUN_TEST(test_refuse_all);
    RUN_TEST(test_box_one_at_a_time);
    RUN_TEST(test_box_expires_after_3_s);
    RUN_TEST(test_box_cancel);
    RUN_TEST(test_box_ticket_never_0);
    return UNITY_END();
}
