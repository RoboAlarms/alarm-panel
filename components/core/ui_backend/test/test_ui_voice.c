/* The touchscreen's voice approval against the engine (ui_backend/ui_voice.h): docs/features/08
 * voice-first-build.md D01, D02, D07; CORE-CFG-021. The panel's port (ui_port_esp.c) and the PC
 * builds (host_panel.c) both approve through these helpers, so the pack checks and the identity
 * checks proved here are the panel backend's too. */
#include <string.h>

#include "sim.h"
#include "ui_backend/ui_voice.h"
#include "unity.h"
#include "vp_test_pack.h"

void setUp(void);
void tearDown(void);
void setUp(void) {}
void tearDown(void) {}

static sim_t S;
static vp_pack_t P; /* the pack the editor leased (revision 1) */

#define W_FRONT 12u
#define W_DOOR 40u
#define W_WINDOW 41u
#define W_LONG 50u   /* 5 s */
#define W_LONGER 51u /* 5 s: with W_LONG and the join, over 10 s */

static void pack_build(void)
{
    static const vpt_clip_t clips[] = {
        {W_FRONT, 1, "Front", "word.front", 0},
        {W_DOOR, 3, "Door", "word.door", 0},
        {W_WINDOW, 3, "Window", "word.window", 0},
        {W_LONG, 3, "Long", "word.long", 80000u},
        {W_LONGER, 3, "Longer", "word.longer", 80000u},
    };
    TEST_ASSERT_EQUAL_INT(VP_OK, vpt_build(clips, sizeof clips / sizeof clips[0], &P));
    TEST_ASSERT_NOT_EQUAL_UINT32(0u, P.pack_revision);
}

static void start(void)
{
    pack_build();
    sim_start(&S, NULL, NULL);
}

static alarm_result_t save_engine(void *ctx, const alarm_config_t *cfg)
{
    (void)ctx;
    return alarm_engine_set_config(&S.eng, cfg, S.now);
}

static const alarm_zone_config_t *zone(uint16_t z)
{
    return &alarm_engine_config(&S.eng)->zones[z - 1u];
}

/* A copy of the stored configuration (the next write's). */
static alarm_config_t *copy(void)
{
    static alarm_config_t cfg;
    cfg = *alarm_engine_config(&S.eng);
    return &cfg;
}

static void set_words(alarm_config_t *cfg, uint16_t z, const uint16_t *w, unsigned n)
{
    for (unsigned i = 0; i < ALARM_ZONE_WORDS; i++) {
        alarm_zone_set_word(&cfg->zones[z - 1u], i, i < n ? w[i] : 0u);
    }
}

/* Someone else's write of the configuration. */
static void write(const alarm_config_t *cfg)
{
    TEST_ASSERT_EQUAL_INT(ALARM_OK, alarm_engine_set_config(&S.eng, cfg, S.now));
}

/* Store words on Z_FRONT through a configuration write (gives it a binding revision). */
static void store_words(const uint16_t *w, unsigned n)
{
    alarm_config_t *cfg = copy();
    set_words(cfg, Z_FRONT, w, n);
    write(cfg);
}

/* What the editor's Done saw: the engine's identity and the leased pack's revision. */
static ui_voice_seen_t seen_now(uint16_t z)
{
    ui_voice_seen_t seen;
    TEST_ASSERT_TRUE(ui_voice_seen_engine(&S.eng, z, &seen));
    seen.pack_revision = P.pack_revision;
    return seen;
}

static uint8_t approve_alone(const ui_voice_seen_t *seen, const uint16_t *w, size_t n)
{
    alarm_result_t saved = ALARM_ERR_BAD_STATE;
    const uint8_t r = ui_voice_apply_approve(&S.eng, &P, NULL, Z_FRONT, seen, w, n, save_engine,
                                             NULL, &saved, S.now);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, saved); /* nothing to save is no failure */
    return r;
}

static uint8_t save_approve(const alarm_config_t *cfg, const ui_voice_seen_t *seen,
                            const uint16_t *w, size_t n, alarm_result_t *saved)
{
    return ui_voice_apply_approve(&S.eng, &P, cfg, Z_FRONT, seen, w, n, save_engine, NULL, saved,
                                  S.now);
}

/* ---- granted ----------------------------------------------------------------------------- */

/* CORE-CFG-021, D07: Done on stored words (a review): approved with the editor's pack revision,
 * against the identity it saw. */
static void test_approve_alone_ok(void)
{
    static const uint16_t w[2] = {W_FRONT, W_DOOR};
    start();
    store_words(w, 2);
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(zone(Z_FRONT)));
    const ui_voice_seen_t seen = seen_now(Z_FRONT);
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_OK, approve_alone(&seen, w, 2));
    TEST_ASSERT_TRUE(alarm_zone_voice_approved(zone(Z_FRONT)));
    TEST_ASSERT_EQUAL_UINT32(P.pack_revision, zone(Z_FRONT)->voice_pack_revision);
}

/* CORE-CFG-021, D07: Done, then the page's save: the draft's words saved and approved in the
 * same transaction. */
static void test_save_and_approve_ok(void)
{
    static const uint16_t w[2] = {W_FRONT, W_DOOR};
    start();
    const ui_voice_seen_t seen = seen_now(Z_FRONT);
    alarm_config_t *draft = copy();
    set_words(draft, Z_FRONT, w, 2);
    alarm_result_t saved = ALARM_ERR_BAD_STATE;
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_OK, save_approve(draft, &seen, w, 2, &saved));
    TEST_ASSERT_EQUAL_INT(ALARM_OK, saved);
    TEST_ASSERT_TRUE(alarm_zone_voice_approved(zone(Z_FRONT)));
    TEST_ASSERT_EQUAL_UINT16(W_DOOR, alarm_zone_word(zone(Z_FRONT), 1));
}

/* D03 "Done approves against the current binding": the installer's own rename in the page Done
 * was tapped on is saved with it, and the approval is against the binding that save produced. */
static void test_own_rename_approves(void)
{
    static const uint16_t w[2] = {W_FRONT, W_DOOR};
    start();
    store_words(w, 2);
    const ui_voice_seen_t seen = seen_now(Z_FRONT);
    alarm_config_t *draft = copy();
    strcpy(draft->zones[Z_FRONT - 1u].name, "Back Door");
    alarm_result_t saved = ALARM_ERR_BAD_STATE;
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_OK, save_approve(draft, &seen, w, 2, &saved));
    TEST_ASSERT_EQUAL_INT(ALARM_OK, saved);
    TEST_ASSERT_NOT_EQUAL_UINT32(seen.binding_revision, zone(Z_FRONT)->voice_binding_revision);
    TEST_ASSERT_TRUE(alarm_zone_voice_approved(zone(Z_FRONT)));
}

/* D03: the installer's own partition move in the same page, likewise. */
static void test_own_move_approves(void)
{
    static const uint16_t w[1] = {W_DOOR};
    start();
    alarm_config_t *cfg = copy();
    cfg->partitions[1].enabled = true;
    set_words(cfg, Z_FRONT, w, 1);
    write(cfg);
    const ui_voice_seen_t seen = seen_now(Z_FRONT);
    alarm_config_t *draft = copy();
    draft->zones[Z_FRONT - 1u].partition = 2;
    alarm_result_t saved = ALARM_ERR_BAD_STATE;
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_OK, save_approve(draft, &seen, w, 1, &saved));
    TEST_ASSERT_EQUAL_INT(ALARM_OK, saved);
    TEST_ASSERT_EQUAL_UINT8(2, zone(Z_FRONT)->partition);
    TEST_ASSERT_TRUE(alarm_zone_voice_approved(zone(Z_FRONT)));
}

/* ---- refused: the identity Done saw is gone ------------------------------------------------ */

/* D07: someone else's rename between Done and the approval: STALE, nothing approved. */
static void test_rename_refuses(void)
{
    static const uint16_t w[2] = {W_FRONT, W_DOOR};
    start();
    store_words(w, 2);
    const ui_voice_seen_t seen = seen_now(Z_FRONT);
    alarm_config_t *cfg = copy();
    strcpy(cfg->zones[Z_FRONT - 1u].name, "Side Door");
    write(cfg);
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_STALE, approve_alone(&seen, w, 2));
    /* the engine half alone, the panel's alarm-task step, refuses it too */
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_STALE,
                            ui_voice_approve_engine(&S.eng, Z_FRONT, &seen, false, w, 2,
                                                    P.pack_revision, S.now));
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(zone(Z_FRONT)));
}

/* D07: someone else's partition move: STALE. */
static void test_partition_move_refuses(void)
{
    static const uint16_t w[2] = {W_FRONT, W_DOOR};
    start();
    alarm_config_t *cfg = copy();
    cfg->partitions[1].enabled = true;
    set_words(cfg, Z_FRONT, w, 2);
    write(cfg);
    const ui_voice_seen_t seen = seen_now(Z_FRONT);
    cfg = copy();
    cfg->zones[Z_FRONT - 1u].partition = 2;
    write(cfg);
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_STALE, approve_alone(&seen, w, 2));
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(zone(Z_FRONT)));
}

/* D07: a changed sensor (a new binding revision, as zone_service writes it): STALE. */
static void test_sensor_change_refuses(void)
{
    static const uint16_t w[2] = {W_FRONT, W_DOOR};
    start();
    store_words(w, 2);
    const ui_voice_seen_t seen = seen_now(Z_FRONT);
    alarm_config_t *cfg = copy();
    uint8_t zones[ALARM_ZONE_BITMAP_BYTES] = {0};
    zones[(Z_FRONT - 1u) / 8u] = (uint8_t)(1u << ((Z_FRONT - 1u) % 8u));
    TEST_ASSERT_EQUAL_UINT(1u, alarm_config_voice_invalidate_zones(cfg, zones));
    write(cfg);
    TEST_ASSERT_NOT_EQUAL_UINT32(seen.binding_revision, zone(Z_FRONT)->voice_binding_revision);
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_STALE, approve_alone(&seen, w, 2));
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(zone(Z_FRONT)));
}

/* D01, D07: the slot removed and used again (same name, same words) is another zone: STALE. */
static void test_slot_reuse_refuses(void)
{
    static const uint16_t w[2] = {W_FRONT, W_DOOR};
    start();
    store_words(w, 2);
    const ui_voice_seen_t seen = seen_now(Z_FRONT);
    const alarm_zone_config_t was = *zone(Z_FRONT);
    alarm_config_t *cfg = copy();
    cfg->zones[Z_FRONT - 1u].type = ALARM_ZT_NOT_USED;
    write(cfg);
    cfg = copy();
    cfg->zones[Z_FRONT - 1u] = was;
    cfg->zones[Z_FRONT - 1u].voice_binding_revision = 0u;
    cfg->zones[Z_FRONT - 1u].voice_approved_revision = 0u;
    write(cfg);
    TEST_ASSERT_EQUAL_UINT16(W_DOOR, alarm_zone_word(zone(Z_FRONT), 1));
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_STALE, approve_alone(&seen, w, 2));
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(zone(Z_FRONT)));
}

/* D07, D09: the pack replaced after the editor showed its words, with the same word IDs: STALE,
 * whether the lease is the new pack or the engine is handed the new revision. */
static void test_pack_replaced_refuses(void)
{
    static const uint16_t w[2] = {W_FRONT, W_DOOR};
    start();
    store_words(w, 2);
    const ui_voice_seen_t seen = seen_now(Z_FRONT);
    vp_pack_t replaced = P;
    replaced.pack_revision = P.pack_revision + 1u;
    alarm_result_t saved = ALARM_ERR_BAD_STATE;
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_STALE,
                            ui_voice_apply_approve(&S.eng, &replaced, NULL, Z_FRONT, &seen, w, 2,
                                                   save_engine, NULL, &saved, S.now));
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_STALE, ui_voice_check_pack(&replaced, &seen, w, 2));
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_STALE,
                            ui_voice_approve_engine(&S.eng, Z_FRONT, &seen, false, w, 2,
                                                    replaced.pack_revision, S.now));
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(zone(Z_FRONT)));
}

/* D07: someone else's save between Done and the page's save (another zone's): the page's save
 * still applies, its result OK, and the approval is refused. */
static void test_other_save_refuses_approval_not_save(void)
{
    static const uint16_t w[2] = {W_FRONT, W_DOOR};
    start();
    const ui_voice_seen_t seen = seen_now(Z_FRONT);
    alarm_config_t *other = copy();
    strcpy(other->zones[Z_GARAGE - 1u].name, "Carport");
    write(other);
    alarm_config_t *draft = copy();
    set_words(draft, Z_FRONT, w, 2);
    alarm_result_t saved = ALARM_ERR_BAD_STATE;
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_STALE, save_approve(draft, &seen, w, 2, &saved));
    TEST_ASSERT_EQUAL_INT(ALARM_OK, saved);
    TEST_ASSERT_EQUAL_UINT16(W_DOOR, alarm_zone_word(zone(Z_FRONT), 1)); /* saved */
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(zone(Z_FRONT)));
}

/* D07: the engine half after a save only when that save was the one change since Done: a
 * second change after it (the panel's steps run in two alarm-task calls) is STALE. */
static void test_engine_half_counts_the_save(void)
{
    static const uint16_t w[2] = {W_FRONT, W_DOOR};
    start();
    const ui_voice_seen_t seen = seen_now(Z_FRONT);
    alarm_config_t *draft = copy();
    set_words(draft, Z_FRONT, w, 2);
    write(draft); /* the page's save: exactly one step */
    TEST_ASSERT_EQUAL_UINT32(seen.generation + 1u, alarm_engine_config_generation(&S.eng));
    alarm_config_t *other = copy();
    strcpy(other->zones[Z_GARAGE - 1u].name, "Carport");
    write(other); /* someone else's, before the approval step */
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_STALE,
                            ui_voice_approve_engine(&S.eng, Z_FRONT, &seen, true, w, 2,
                                                    P.pack_revision, S.now));
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(zone(Z_FRONT)));
    /* and without a save, a moved generation is stale too */
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_STALE,
                            ui_voice_approve_engine(&S.eng, Z_FRONT, &seen, false, w, 2,
                                                    P.pack_revision, S.now));
}

/* ---- refused: the words ------------------------------------------------------------------ */

/* CORE-CFG-021: words other than the stored ones (changed, shorter, longer) are STALE. */
static void test_stale_words(void)
{
    static const uint16_t w[3] = {W_FRONT, W_DOOR, W_WINDOW};
    static const uint16_t other[2] = {W_FRONT, W_WINDOW};
    start();
    store_words(w, 2);
    const ui_voice_seen_t seen = seen_now(Z_FRONT);
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_STALE, approve_alone(&seen, other, 2));
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_STALE, approve_alone(&seen, w, 1));
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_STALE, approve_alone(&seen, w, 3));
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(zone(Z_FRONT)));
}

/* D02, D07 (review 3 finding 5): an ID the pack doesn't have, a status clip and a spoken name
 * over 10 s are refused - and with a save the draft is still stored, only unapproved. */
static void test_unsayable_words_refuse(void)
{
    static const uint16_t unknown[2] = {W_FRONT, 0x7FFEu};
    static const uint16_t status[1] = {VP_STATUS_FIRST};
    static const uint16_t overlong[2] = {W_LONG, W_LONGER};
    static const uint16_t fits[1] = {W_LONG};
    start();
    const ui_voice_seen_t seen = seen_now(Z_FRONT);
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_UNAVAILABLE, ui_voice_check_pack(&P, &seen, unknown, 2));
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_UNAVAILABLE, ui_voice_check_pack(&P, &seen, status, 1));
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_UNAVAILABLE, ui_voice_check_pack(&P, &seen, overlong, 2));
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_OK, ui_voice_check_pack(&P, &seen, fits, 1));

    alarm_config_t *draft = copy();
    set_words(draft, Z_FRONT, unknown, 2);
    alarm_result_t saved = ALARM_ERR_BAD_STATE;
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_UNAVAILABLE, save_approve(draft, &seen, unknown, 2, &saved));
    TEST_ASSERT_EQUAL_INT(ALARM_OK, saved);
    TEST_ASSERT_EQUAL_UINT16(0x7FFEu, alarm_zone_word(zone(Z_FRONT), 1)); /* a draft, unapproved */
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(zone(Z_FRONT)));

    const ui_voice_seen_t again = seen_now(Z_FRONT);
    draft = copy();
    set_words(draft, Z_FRONT, overlong, 2);
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_UNAVAILABLE, save_approve(draft, &again, overlong, 2, &saved));
    TEST_ASSERT_EQUAL_INT(ALARM_OK, saved);
    TEST_ASSERT_EQUAL_UINT16(W_LONGER, alarm_zone_word(zone(Z_FRONT), 1));
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(zone(Z_FRONT)));
}

/* CORE-CFG-021: no lease, no identity, no words, too many, a zone out of range or no engine:
 * UNAVAILABLE, nothing approved. */
static void test_unavailable(void)
{
    static const uint16_t w[ALARM_ZONE_WORDS + 1] = {W_FRONT, W_DOOR, 1, 1, 1, 1, 1, 1, 1};
    start();
    store_words(w, 2);
    const ui_voice_seen_t seen = seen_now(Z_FRONT);
    alarm_result_t saved;
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_UNAVAILABLE,
                            ui_voice_apply_approve(&S.eng, NULL, NULL, Z_FRONT, &seen, w, 2,
                                                   save_engine, NULL, &saved, S.now));
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_UNAVAILABLE,
                            ui_voice_apply_approve(&S.eng, &P, NULL, Z_FRONT, NULL, w, 2,
                                                   save_engine, NULL, &saved, S.now));
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_UNAVAILABLE, approve_alone(&seen, w, 0));
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_UNAVAILABLE, approve_alone(&seen, NULL, 2));
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_UNAVAILABLE,
                            ui_voice_approve_engine(&S.eng, Z_FRONT, &seen, false, w, 2, 0u, S.now));
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_UNAVAILABLE,
                            ui_voice_approve_engine(&S.eng, Z_FRONT, &seen, false, w,
                                                    ALARM_ZONE_WORDS + 1, P.pack_revision, S.now));
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_UNAVAILABLE,
                            ui_voice_approve_engine(&S.eng, 0, &seen, false, w, 2, P.pack_revision, S.now));
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_UNAVAILABLE,
                            ui_voice_approve_engine(&S.eng, ALARM_MAX_ZONES + 1, &seen, false, w, 2,
                                                    P.pack_revision, S.now));
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_UNAVAILABLE,
                            ui_voice_approve_engine(NULL, Z_FRONT, &seen, false, w, 2, P.pack_revision, S.now));
    ui_voice_seen_t out;
    TEST_ASSERT_FALSE(ui_voice_seen_engine(&S.eng, 0, &out));
    TEST_ASSERT_FALSE(ui_voice_seen_engine(&S.eng, ALARM_MAX_ZONES + 1, &out));
    TEST_ASSERT_FALSE(ui_voice_seen_engine(NULL, Z_FRONT, &out));
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(zone(Z_FRONT)));
}

/* CORE-CFG-021: approval only while every partition is idle - armed is REFUSED; a save refused
 * the same way reports its own result and grants nothing. */
static void test_refused_when_armed(void)
{
    static const uint16_t w[2] = {W_FRONT, W_DOOR};
    start();
    store_words(w, 2);
    const ui_voice_seen_t seen = seen_now(Z_FRONT);
    sim_arm_away_and_leave(&S, "1111");
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_REFUSED, approve_alone(&seen, w, 2));
    alarm_config_t *draft = copy();
    alarm_result_t saved = ALARM_OK;
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_REFUSED, save_approve(draft, &seen, w, 2, &saved));
    TEST_ASSERT_NOT_EQUAL_INT(ALARM_OK, saved);
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(zone(Z_FRONT)));
}

/* Every engine answer has a UI answer. */
static void test_result_map(void)
{
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_OK, ui_voice_approve_result(ALARM_VOICE_OK));
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_UNAVAILABLE, ui_voice_approve_result(ALARM_VOICE_BAD_ARG));
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_STALE, ui_voice_approve_result(ALARM_VOICE_STALE));
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_UNAVAILABLE, ui_voice_approve_result(ALARM_VOICE_UNAVAILABLE));
    TEST_ASSERT_EQUAL_UINT8(UI_VOICE_APPROVE_REFUSED, ui_voice_approve_result(ALARM_VOICE_BUSY));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_approve_alone_ok);
    RUN_TEST(test_save_and_approve_ok);
    RUN_TEST(test_own_rename_approves);
    RUN_TEST(test_own_move_approves);
    RUN_TEST(test_rename_refuses);
    RUN_TEST(test_partition_move_refuses);
    RUN_TEST(test_sensor_change_refuses);
    RUN_TEST(test_slot_reuse_refuses);
    RUN_TEST(test_pack_replaced_refuses);
    RUN_TEST(test_other_save_refuses_approval_not_save);
    RUN_TEST(test_engine_half_counts_the_save);
    RUN_TEST(test_stale_words);
    RUN_TEST(test_unsayable_words_refuse);
    RUN_TEST(test_unavailable);
    RUN_TEST(test_refused_when_armed);
    RUN_TEST(test_result_map);
    return UNITY_END();
}
