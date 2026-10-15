/*
 * test_config.c - configuration defaults and validation, installer config
 * changes, zone type rules, stable names.
 * Requirements: CORE-CFG-*
 */
#include <stdio.h>

#include "sim.h"

static sim_t S;
static alarm_config_t C;

void setUp(void)
{
    sim_config_basic(&C);
}

void tearDown(void) {}

static alarm_result_t validate(void)
{
    return alarm_config_validate(&C, NULL, 0);
}

/* CORE-CFG-001 */
static void test_defaults_are_valid(void)
{
    alarm_config_t d;
    alarm_config_defaults(&d);
    char why[96] = "x";
    EXPECT_RESULT(ALARM_OK, alarm_config_validate(&d, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("", why);
    TEST_ASSERT_EQUAL_UINT16(60, d.partitions[0].exit_delay_s);
    TEST_ASSERT_EQUAL_UINT16(30, d.partitions[0].entry_delay1_s);
    TEST_ASSERT_EQUAL_UINT8(30, d.opt.abort_window_s);
    TEST_ASSERT_EQUAL_UINT16(300, d.opt.cancel_window_s);
    TEST_ASSERT_EQUAL_UINT8(2, d.opt.swinger_trips);
    TEST_ASSERT_TRUE(d.opt.auto_stay);
    TEST_ASSERT_TRUE(d.partitions[0].enabled);
    TEST_ASSERT_FALSE(d.partitions[1].enabled);
}

/* CORE-CFG-002: SIA CP-01 ranges enforced unless relaxed. */
static void test_cp01_ranges(void)
{
    C.partitions[0].exit_delay_s = 30;
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, validate());
    C.opt.relaxed_limits = true;
    EXPECT_RESULT(ALARM_OK, validate());

    sim_config_basic(&C);
    C.partitions[0].entry_delay1_s = 20;
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, validate());

    sim_config_basic(&C);
    C.opt.abort_window_s = 60;
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, validate());

    sim_config_basic(&C);
    C.opt.cancel_window_s = 100;
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, validate());

    sim_config_basic(&C);
    C.opt.swinger_trips = 0;
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, validate());
    C.opt.swinger_trips = 7;
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, validate());
    C.opt.relaxed_limits = true;
    EXPECT_RESULT(ALARM_OK, validate());

    sim_config_basic(&C);
    C.opt.code_min_len = 3;
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, validate());
    C.opt.code_min_len = 11;
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, validate());
    C.opt.code_min_len = 6;
    EXPECT_RESULT(ALARM_OK, validate());
}

/* CORE-CFG-003 */
static void test_zone_validation(void)
{
    char why[96];
    C.zones[40].type = (alarm_zone_type_t)99;
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_validate(&C, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("zone 41: has an invalid type", why);

    sim_config_basic(&C);
    C.zones[Z_WINDOW - 1].partition = 2; /* partition 2 disabled */
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, validate());

    sim_config_basic(&C);
    C.zones[Z_WINDOW - 1].cross_zone = Z_WINDOW;
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, validate());

    sim_config_basic(&C);
    C.zones[Z_FOB_AWAY - 1].user = 0;
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, validate());
}

/* CORE-CFG-004 */
static void test_user_validation(void)
{
    uint8_t h[ALARM_CODE_HASH_LEN];
    sim_hash(NULL, C.code_salt, "4242", h);
    alarm_config_set_user(&C, 1, ALARM_AUTH_STANDARD, 0x01, "x", h);
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, validate());

    sim_config_basic(&C);
    sim_set_user(&C, 8, ALARM_AUTH_STANDARD, 0x01, CODE_USER); /* duplicate code */
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, validate());

    sim_config_basic(&C);
    sim_set_user(&C, 8, ALARM_AUTH_MASTER, 0x01, "4242");
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, validate());

    sim_config_basic(&C);
    sim_set_user(&C, 8, ALARM_AUTH_STANDARD, 0x00, "4242");
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, validate());

    sim_config_basic(&C);
    C.partitions[0].enabled = false;
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, validate());
}

/* CORE-CFG-005: configuration changes only while everything is disarmed. */
static void test_set_config_requires_idle(void)
{
    sim_start(&S, NULL, NULL);
    alarm_config_t cfg = S.cfg;
    cfg.partitions[0].exit_delay_s = 90;
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    EXPECT_RESULT(ALARM_ERR_BAD_STATE, alarm_engine_set_config(&S.eng, &cfg, S.now));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_RESULT(ALARM_OK, alarm_engine_set_config(&S.eng, &cfg, S.now));
    EXPECT_REPORTS(&S, ALARM_EV_CONFIG_CHANGED, 1);
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_STAY, 0));
    TEST_ASSERT_EQUAL_UINT64(SEC(90), sim_status(&S, 1).delay_remaining_ms);

    cfg.partitions[0].exit_delay_s = 5; /* invalid (CP-01) */
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_engine_set_config(&S.eng, &cfg, S.now));
}

/* CORE-CFG-006: changing a zone's type resets its runtime state. */
static void test_set_config_resets_changed_zone(void)
{
    sim_start(&S, NULL, NULL);
    EXPECT_RESULT(ALARM_OK, sim_bypass(&S, 1, CODE_USER, Z_WINDOW));
    EXPECT_RESULT(ALARM_OK, sim_bypass(&S, 1, CODE_USER, Z_HALL_PIR));
    alarm_config_t cfg = S.cfg;
    cfg.zones[Z_WINDOW - 1].type = ALARM_ZT_ENTRY_EXIT_1;
    EXPECT_RESULT(ALARM_OK, alarm_engine_set_config(&S.eng, &cfg, S.now));
    EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_BYPASSED);
    EXPECT_ZONE_FLAG(&S, Z_HALL_PIR, ALARM_ZS_BYPASSED);
}

/* CORE-CFG-018: alarm_zone_config_resets_state names exactly the zones whose troubles (and
 * source CHECK) a configuration change makes the engine forget: new, removed, retyped or moved
 * zones, not renamed ones. */
static void test_resets_state_rule(void)
{
    sim_start(&S, NULL, NULL);
    sim_zev(&S, Z_WINDOW, ALARM_ZEV_UNREACHABLE);
    sim_zev(&S, Z_HALL_PIR, ALARM_ZEV_UNREACHABLE);
    sim_zev(&S, Z_FAMILY, ALARM_ZEV_UNREACHABLE);
    alarm_config_t cfg = S.cfg;
    cfg.zones[Z_WINDOW - 1].type = ALARM_ZT_ENTRY_EXIT_1;
    snprintf(cfg.zones[Z_HALL_PIR - 1].name, ALARM_NAME_LEN, "%s", "Hall PIR");
    cfg.zones[Z_FAMILY - 1].flags ^= ALARM_ZF_CHIME;
    alarm_zone_config_defaults(&cfg.zones[40], 41, ALARM_ZT_PERIMETER, 1);
    TEST_ASSERT_TRUE(alarm_zone_config_resets_state(&S.cfg.zones[Z_WINDOW - 1],
                                                    &cfg.zones[Z_WINDOW - 1]));
    TEST_ASSERT_FALSE(alarm_zone_config_resets_state(&S.cfg.zones[Z_HALL_PIR - 1],
                                                     &cfg.zones[Z_HALL_PIR - 1]));
    TEST_ASSERT_FALSE(alarm_zone_config_resets_state(&S.cfg.zones[Z_FAMILY - 1],
                                                     &cfg.zones[Z_FAMILY - 1]));
    TEST_ASSERT_TRUE(alarm_zone_config_resets_state(&S.cfg.zones[40], &cfg.zones[40]));
    EXPECT_RESULT(ALARM_OK, alarm_engine_set_config(&S.eng, &cfg, S.now));
    EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_SUPERVISION);
    EXPECT_ZONE_FLAG(&S, Z_HALL_PIR, ALARM_ZS_SUPERVISION);
    EXPECT_ZONE_FLAG(&S, Z_FAMILY, ALARM_ZS_SUPERVISION);
    sim_fault(&S, Z_WINDOW); /* its CHECK went with the reset: inputs count normally */
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_FAULT);
    alarm_config_t moved = cfg;
    moved.partitions[1].enabled = true;
    moved.zones[Z_HALL_PIR - 1].partition = 2;
    TEST_ASSERT_TRUE(alarm_zone_config_resets_state(&cfg.zones[Z_HALL_PIR - 1],
                                                    &moved.zones[Z_HALL_PIR - 1]));

    /* A retyped zone keeps its sensor's state; a removed zone doesn't come back open. */
    cfg.zones[Z_WINDOW - 1].type = ALARM_ZT_PERIMETER;
    EXPECT_RESULT(ALARM_OK, alarm_engine_set_config(&S.eng, &cfg, S.now));
    EXPECT_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_FAULT);
    alarm_config_t removed = cfg;
    memset(&removed.zones[Z_WINDOW - 1], 0, sizeof(removed.zones[0]));
    EXPECT_RESULT(ALARM_OK, alarm_engine_set_config(&S.eng, &removed, S.now));
    EXPECT_RESULT(ALARM_OK, alarm_engine_set_config(&S.eng, &cfg, S.now));
    EXPECT_NO_ZONE_FLAG(&S, Z_WINDOW, ALARM_ZS_FAULT);
}

/* CORE-CFG-007 */
static void test_init_rejects_bad_input(void)
{
    alarm_engine_t *e = &S.eng;
    alarm_ports_t ports;
    memset(&ports, 0, sizeof(ports));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_engine_init(e, &C, &ports, NULL, 0)); /* no hash */
    ports.hash_code = sim_hash;
    C.opt.code_min_len = 12;
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_engine_init(e, &C, &ports, NULL, 0));
    sim_config_basic(&C);
    EXPECT_RESULT(ALARM_OK, alarm_engine_init(e, &C, &ports, NULL, 0));
}

/* CORE-CFG-008 */
static void test_zone_defaults(void)
{
    alarm_zone_config_t z;
    alarm_zone_config_defaults(&z, 7, ALARM_ZT_ENTRY_EXIT_1, 1);
    TEST_ASSERT_TRUE((z.flags & ALARM_ZF_CHIME) != 0u);
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_DOOR, z.device_class);
    TEST_ASSERT_EQUAL_STRING("Zone 007", z.name);
    alarm_zone_config_defaults(&z, 8, ALARM_ZT_INTERIOR_FOLLOWER, 1);
    TEST_ASSERT_EQUAL_UINT16(0, z.flags);
    TEST_ASSERT_EQUAL_INT(ALARM_DEVCLASS_MOTION, z.device_class);
}

/* CORE-CFG-009: which zone types are armed in which level. */
static void test_armed_in_level_table(void)
{
    alarm_zone_config_t z;
    alarm_zone_config_defaults(&z, 1, ALARM_ZT_PERIMETER, 1);
    TEST_ASSERT_TRUE(alarm_zone_armed_in_level(&z, ALARM_LEVEL_STAY));
    TEST_ASSERT_TRUE(alarm_zone_armed_in_level(&z, ALARM_LEVEL_NIGHT));
    alarm_zone_config_defaults(&z, 1, ALARM_ZT_INTERIOR_FOLLOWER, 1);
    TEST_ASSERT_TRUE(alarm_zone_armed_in_level(&z, ALARM_LEVEL_AWAY));
    TEST_ASSERT_FALSE(alarm_zone_armed_in_level(&z, ALARM_LEVEL_STAY));
    TEST_ASSERT_FALSE(alarm_zone_armed_in_level(&z, ALARM_LEVEL_NIGHT));
    z.flags |= ALARM_ZF_NIGHT;
    TEST_ASSERT_TRUE(alarm_zone_armed_in_level(&z, ALARM_LEVEL_NIGHT));
    alarm_zone_config_defaults(&z, 1, ALARM_ZT_FIRE, 1);
    TEST_ASSERT_FALSE(alarm_zone_armed_in_level(&z, ALARM_LEVEL_AWAY));
    TEST_ASSERT_FALSE(alarm_zone_bypassable(&z));
}

/* CORE-CFG-010: every enum value has a stable name. */
static void test_names(void)
{
    for (int t = 0; t < ALARM_EV_COUNT; t++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("unknown", alarm_event_name((alarm_event_type_t)t)));
    }
    for (int c = 0; c < ALARM_CAUSE_COUNT; c++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("unknown", alarm_cause_name((alarm_cause_t)c)));
    }
    for (int t = 0; t < ALARM_TRBL_COUNT; t++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("unknown", alarm_sys_trouble_name((alarm_sys_trouble_t)t)));
    }
    for (int t = 0; t < 100; t++) {
        if (alarm_zone_type_valid(t)) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp("unknown", alarm_zone_type_name((alarm_zone_type_t)t)));
        }
    }
    /* Every result carries a name: they go out over MQTT and into the event log. Extend the
     * bound whenever a result is appended, or the new one silently goes unnamed. */
    for (int r = 0; r <= ALARM_ERR_UNAVAILABLE; r++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("unknown", alarm_result_name((alarm_result_t)r)));
    }
    TEST_ASSERT_EQUAL_STRING("unavailable", alarm_result_name(ALARM_ERR_UNAVAILABLE));
    TEST_ASSERT_EQUAL_STRING("entry_exit_1", alarm_zone_type_name(ALARM_ZT_ENTRY_EXIT_1));
    TEST_ASSERT_EQUAL_STRING("unknown", alarm_event_name((alarm_event_type_t)250));
}

/* CORE-CFG-019: the check answers with a code and a number a screen words in its own language;
 * the English reason is the same check, word for word as before. */
static void test_check_codes(void)
{
    alarm_config_why_t w;
    EXPECT_RESULT(ALARM_OK, alarm_config_check(&C, &w));
    TEST_ASSERT_EQUAL_INT(ALARM_CFG_FINE, w.problem);

    char why[96];
    C.zones[40].type = (alarm_zone_type_t)99;
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_check(&C, &w));
    TEST_ASSERT_EQUAL_INT(ALARM_CFG_ZONE_TYPE, w.problem);
    TEST_ASSERT_EQUAL_UINT(41, w.number);
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_validate(&C, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("zone 41: has an invalid type", why);

    sim_config_basic(&C);
    C.zones[Z_WINDOW - 1].partition = 2; /* partition 2 disabled */
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_check(&C, &w));
    TEST_ASSERT_EQUAL_INT(ALARM_CFG_ZONE_PARTITION, w.problem);
    TEST_ASSERT_EQUAL_UINT(Z_WINDOW, w.number);

    sim_config_basic(&C);
    C.opt.fire_verify_window_s = 0;
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_check(&C, &w));
    TEST_ASSERT_EQUAL_INT(ALARM_CFG_FIRE_TIMES, w.problem);
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_validate(&C, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("fire verify window and smoke reset must be > 0", why);

    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_check(NULL, &w));
    TEST_ASSERT_EQUAL_INT(ALARM_CFG_NULL, w.problem);
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_check(NULL, NULL)); /* why is optional */
    /* stored nowhere, but a screen's table follows these numbers: never renumber */
    TEST_ASSERT_EQUAL_INT(1, ALARM_CFG_NULL);
    TEST_ASSERT_EQUAL_INT(27, ALARM_CFG_DEVICE_PARTITION);
    TEST_ASSERT_EQUAL_INT(28, ALARM_CFG_ZONE_WORDS); /* CORE-CFG-020, appended */
}

static void set_words(alarm_zone_config_t *z, const uint16_t *w, unsigned n)
{
    memset(z->spoken_words, 0, sizeof(z->spoken_words));
    for (unsigned i = 0; i < n; i++) {
        alarm_zone_set_word(z, i, w[i]);
    }
}

/* CORE-CFG-020: a zone's spoken name is up to eight token IDs, two little-endian bytes each,
 * a nonzero prefix then zeros; repeats are fine, holes and words a name can't choose are not. */
static void test_zone_words_shape(void)
{
    alarm_zone_config_t *z = &C.zones[Z_FRONT - 1];
    TEST_ASSERT_EQUAL_UINT(0, alarm_zone_word_count(z)); /* no spoken name */
    TEST_ASSERT_TRUE(alarm_zone_words_valid(z));
    alarm_zone_set_word(z, 0, 0x1234u);
    TEST_ASSERT_EQUAL_UINT8(0x34, z->spoken_words[0]);
    TEST_ASSERT_EQUAL_UINT8(0x12, z->spoken_words[1]);
    TEST_ASSERT_EQUAL_UINT16(0x1234u, alarm_zone_word(z, 0));
    alarm_zone_set_word(z, ALARM_ZONE_WORDS, 7u); /* out of range: nothing */
    TEST_ASSERT_EQUAL_UINT16(0u, alarm_zone_word(z, ALARM_ZONE_WORDS));
    TEST_ASSERT_EQUAL_UINT16(0u, alarm_zone_word(NULL, 0));

    static const uint16_t eight[8] = {5, 5, 5, 5, 5, 5, 5, ALARM_VOICE_TOKEN_SELECTABLE_MAX};
    set_words(z, eight, 8);
    TEST_ASSERT_EQUAL_UINT(8, alarm_zone_word_count(z));
    EXPECT_RESULT(ALARM_OK, validate());

    alarm_config_why_t w;
    char why[96];
    alarm_zone_set_word(z, 3, 0u); /* a hole */
    TEST_ASSERT_FALSE(alarm_zone_words_valid(z));
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_check(&C, &w));
    TEST_ASSERT_EQUAL_INT(ALARM_CFG_ZONE_WORDS, w.problem);
    TEST_ASSERT_EQUAL_UINT(Z_FRONT, w.number);
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_validate(&C, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("zone 1: has an invalid spoken name", why);

    static const uint16_t status[1] = {0x8001u}; /* "arming": not a word a name can choose */
    set_words(z, status, 1);
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_config_check(&C, &w));
    TEST_ASSERT_EQUAL_INT(ALARM_CFG_ZONE_WORDS, w.problem);

    /* A zone not in use isn't checked (nor written). */
    sim_config_basic(&C);
    C.zones[ALARM_MAX_ZONES - 1].spoken_words[2] = 9;
    EXPECT_RESULT(ALARM_OK, validate());
}

static void words_of(const alarm_zone_config_t *z, uint16_t w[ALARM_ZONE_WORDS])
{
    for (unsigned i = 0; i < ALARM_ZONE_WORDS; i++) {
        w[i] = alarm_zone_word(z, i);
    }
}

/* The approval transaction as the touchscreen's Done calls it: the binding and words shown. */
static alarm_voice_result_t approve_shown(alarm_config_t *cfg, uint16_t zone, uint32_t pack)
{
    uint16_t w[ALARM_ZONE_WORDS];
    words_of(&cfg->zones[zone - 1u], w);
    return alarm_config_voice_approve(cfg, zone, cfg->zones[zone - 1u].voice_binding_revision, w,
                                      pack);
}

/* CORE-CFG-021: the voice revisions follow the zone's identity, never the writer's say-so; a
 * write can never grant, replace or bring back approval (D07). */
static void test_voice_reconcile_rules(void)
{
    static alarm_config_t old, next, approved_a;
    static const uint16_t front[2] = {12, 40};
    alarm_zone_config_t *z = &next.zones[Z_FRONT - 1];
    const alarm_zone_config_t *o = &old.zones[Z_FRONT - 1];

    old = C; /* a migrated configuration: no names, no revisions */
    next = old;
    alarm_config_voice_reconcile(&old, &next); /* nothing to do */
    TEST_ASSERT_EQUAL_MEMORY(&old, &next, sizeof(old));

    /* The first voice edit of a migrated zone gives it an identity; no approval with it. */
    set_words(z, front, 2);
    z->voice_approved_revision = 1u; /* a client guessing the revision: not trusted */
    z->voice_pack_revision = 3u;
    alarm_config_voice_reconcile(&old, &next);
    TEST_ASSERT_EQUAL_UINT32(1u, z->voice_binding_revision);
    TEST_ASSERT_EQUAL_UINT32(0u, z->voice_approved_revision);
    TEST_ASSERT_EQUAL_UINT32(0u, z->voice_pack_revision);
    TEST_ASSERT_EQUAL_UINT32(1u, next.opt.voice_revision_counter);

    /* An approval made on a copy and written back is not one: only the transaction on the
     * stored configuration grants it. */
    old = next;
    TEST_ASSERT_EQUAL(ALARM_VOICE_OK, approve_shown(&next, Z_FRONT, 7u));
    alarm_config_voice_reconcile(&old, &next);
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(z));
    TEST_ASSERT_EQUAL_UINT32(0u, z->voice_pack_revision);
    TEST_ASSERT_EQUAL(ALARM_VOICE_OK, approve_shown(&old, Z_FRONT, 7u)); /* the transaction */
    next = old;

    /* A type change of the same zone keeps everything. */
    z->type = ALARM_ZT_PERIMETER;
    alarm_config_voice_reconcile(&old, &next);
    TEST_ASSERT_TRUE(alarm_zone_voice_approved(z));
    TEST_ASSERT_EQUAL_UINT32(1u, z->voice_binding_revision);
    TEST_ASSERT_EQUAL_UINT32(7u, z->voice_pack_revision);

    /* Whatever a write carries in the approval fields - forged, a newer pack, or zeros from a
     * writer that doesn't know them - the stored approval stands. */
    old = next;
    z->voice_approved_revision = 99u;
    alarm_config_voice_reconcile(&old, &next);
    TEST_ASSERT_EQUAL_UINT32(1u, z->voice_approved_revision);
    TEST_ASSERT_EQUAL_UINT32(7u, z->voice_pack_revision);
    z->voice_pack_revision = 8u;
    alarm_config_voice_reconcile(&old, &next);
    TEST_ASSERT_EQUAL_UINT32(7u, z->voice_pack_revision);
    z->voice_approved_revision = 0u;
    z->voice_pack_revision = 0u;
    alarm_config_voice_reconcile(&old, &next);
    TEST_ASSERT_TRUE(alarm_zone_voice_approved(z));
    TEST_ASSERT_EQUAL_UINT32(7u, z->voice_pack_revision);

    /* Stale approval replay: withdrawn explicitly, then the approved configuration written
     * again - not approved. */
    approved_a = next;
    TEST_ASSERT_EQUAL(ALARM_VOICE_OK, alarm_config_voice_withdraw(&old, Z_FRONT, 1u));
    next = approved_a;
    alarm_config_voice_reconcile(&old, &next);
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(z));
    TEST_ASSERT_EQUAL_UINT32(0u, z->voice_pack_revision);
    TEST_ASSERT_EQUAL_UINT32(1u, z->voice_binding_revision);

    /* Recipe A -> B -> A, then the approved A replayed: the binding never changed, and still
     * no approval comes back. */
    TEST_ASSERT_EQUAL(ALARM_VOICE_OK, approve_shown(&old, Z_FRONT, 8u));
    approved_a = old;
    next = old;
    alarm_zone_set_word(z, 2, 12u); /* B */
    alarm_config_voice_reconcile(&old, &next);
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(z));
    TEST_ASSERT_EQUAL_UINT32(1u, z->voice_binding_revision); /* same zone, same identity */
    TEST_ASSERT_EQUAL_UINT(3, alarm_zone_word_count(z));
    old = next;
    next = approved_a; /* A again, carrying A's old approval */
    alarm_config_voice_reconcile(&old, &next);
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(z));
    old = next;
    next = approved_a; /* and once more, now that the words match */
    alarm_config_voice_reconcile(&old, &next);
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(z));
    TEST_ASSERT_EQUAL_UINT32(1u, z->voice_binding_revision);
    TEST_ASSERT_EQUAL_UINT32(0u, z->voice_approved_revision);
    TEST_ASSERT_EQUAL_UINT32(0u, z->voice_pack_revision);

    /* A rename keeps the words, takes a new identity, clears approval; renaming back doesn't
     * bring it back. An approval sent with the rename is dropped. */
    TEST_ASSERT_EQUAL(ALARM_VOICE_OK, approve_shown(&old, Z_FRONT, 8u));
    next = old;
    char name[ALARM_NAME_LEN];
    memcpy(name, z->name, sizeof(name));
    snprintf(z->name, sizeof(z->name), "%s", "Porch");
    alarm_config_voice_reconcile(&old, &next);
    TEST_ASSERT_EQUAL_UINT32(2u, z->voice_binding_revision);
    TEST_ASSERT_EQUAL_UINT32(0u, z->voice_approved_revision);
    TEST_ASSERT_EQUAL_UINT(2, alarm_zone_word_count(z));
    old = next;
    memcpy(z->name, name, sizeof(name));
    z->voice_approved_revision = 1u; /* the old approval, offered back */
    alarm_config_voice_reconcile(&old, &next);
    TEST_ASSERT_EQUAL_UINT32(3u, z->voice_binding_revision);
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(z));

    /* A partition move is a new identity too. */
    old = next;
    TEST_ASSERT_EQUAL(ALARM_VOICE_OK, approve_shown(&old, Z_FRONT, 8u));
    next = old;
    next.partitions[1].enabled = true;
    z->partition = 2;
    alarm_config_voice_reconcile(&old, &next);
    TEST_ASSERT_EQUAL_UINT32(4u, z->voice_binding_revision);
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(z));

    /* A client's binding revision isn't taken: a different one means a new identity. */
    old = next;
    z->voice_binding_revision = 1u;
    alarm_config_voice_reconcile(&old, &next);
    TEST_ASSERT_EQUAL_UINT32(5u, z->voice_binding_revision);
    /* nor its counter */
    old = next;
    next.opt.voice_revision_counter = 0u;
    alarm_config_voice_reconcile(&old, &next);
    TEST_ASSERT_EQUAL_UINT32(5u, next.opt.voice_revision_counter);
    next.opt.voice_revision_counter = UINT32_MAX;
    alarm_config_voice_reconcile(&old, &next);
    TEST_ASSERT_EQUAL_UINT32(5u, next.opt.voice_revision_counter);

    /* Not used removes the name and revisions; the counter survives; the slot used again is a
     * new identity, and an approval sent with it is dropped. */
    old = next;
    z->type = ALARM_ZT_NOT_USED;
    alarm_config_voice_reconcile(&old, &next);
    TEST_ASSERT_EQUAL_UINT(0, alarm_zone_word_count(z));
    TEST_ASSERT_EQUAL_UINT32(0u, z->voice_binding_revision);
    TEST_ASSERT_EQUAL_UINT32(0u, z->voice_approved_revision);
    TEST_ASSERT_EQUAL_UINT32(0u, z->voice_pack_revision);
    TEST_ASSERT_EQUAL_UINT32(5u, next.opt.voice_revision_counter);
    old = next;
    z->type = ALARM_ZT_ENTRY_EXIT_1;
    set_words(z, front, 2);
    z->voice_binding_revision = 6u;
    z->voice_approved_revision = 6u;
    z->voice_pack_revision = 8u;
    alarm_config_voice_reconcile(&old, &next);
    TEST_ASSERT_EQUAL_UINT32(6u, z->voice_binding_revision);
    TEST_ASSERT_EQUAL_UINT32(0u, z->voice_approved_revision);
    TEST_ASSERT_EQUAL_UINT(2, alarm_zone_word_count(z)); /* the draft words stay */
    TEST_ASSERT_TRUE(z->voice_binding_revision > o->voice_binding_revision);
}

/* CORE-CFG-021: the approval and withdrawal transactions - what they refuse (a stale binding
 * or words included), and a counter that never wraps. */
static void test_voice_approve_and_exhaustion(void)
{
    static alarm_config_t old, next;
    static const uint16_t front[1] = {12};
    alarm_zone_config_t *z = &C.zones[Z_FRONT - 1];
    uint16_t w[ALARM_ZONE_WORDS] = {12};

    TEST_ASSERT_EQUAL(ALARM_VOICE_BAD_ARG, alarm_config_voice_approve(NULL, Z_FRONT, 1u, w, 1u));
    TEST_ASSERT_EQUAL(ALARM_VOICE_BAD_ARG, alarm_config_voice_approve(&C, Z_FRONT, 1u, NULL, 1u));
    TEST_ASSERT_EQUAL(ALARM_VOICE_BAD_ARG, alarm_config_voice_approve(&C, 0, 1u, w, 1u));
    TEST_ASSERT_EQUAL(ALARM_VOICE_BAD_ARG,
                      alarm_config_voice_approve(&C, ALARM_MAX_ZONES + 1, 1u, w, 1u));
    TEST_ASSERT_EQUAL(ALARM_VOICE_BAD_ARG,
                      alarm_config_voice_approve(&C, ALARM_MAX_ZONES, 1u, w, 1u)); /* unused */
    set_words(z, front, 1);
    TEST_ASSERT_EQUAL(ALARM_VOICE_UNAVAILABLE, alarm_config_voice_approve(&C, Z_FRONT, 0u, w, 1u));
    z->voice_binding_revision = 4u;
    C.opt.voice_revision_counter = 4u;
    TEST_ASSERT_EQUAL(ALARM_VOICE_BAD_ARG, alarm_config_voice_approve(&C, Z_FRONT, 4u, w, 0u));
    TEST_ASSERT_EQUAL(ALARM_VOICE_STALE, alarm_config_voice_approve(&C, Z_FRONT, 3u, w, 1u));
    w[1] = 40u; /* words the zone doesn't have */
    TEST_ASSERT_EQUAL(ALARM_VOICE_STALE, alarm_config_voice_approve(&C, Z_FRONT, 4u, w, 1u));
    w[1] = 0u;
    alarm_zone_set_word(z, 2, 9u); /* a hole, shown as it is */
    w[2] = 9u;
    TEST_ASSERT_EQUAL(ALARM_VOICE_BAD_ARG, alarm_config_voice_approve(&C, Z_FRONT, 4u, w, 1u));
    alarm_zone_set_word(z, 2, 0u);
    w[2] = 0u;
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(z));
    TEST_ASSERT_EQUAL(ALARM_VOICE_OK, alarm_config_voice_approve(&C, Z_FRONT, 4u, w, 1u));
    TEST_ASSERT_TRUE(alarm_zone_voice_approved(z));
    TEST_ASSERT_EQUAL_UINT32(4u, C.opt.voice_revision_counter); /* nothing else moves */

    /* Withdrawal: explicit, for the binding shown. */
    TEST_ASSERT_EQUAL(ALARM_VOICE_BAD_ARG, alarm_config_voice_withdraw(NULL, Z_FRONT, 4u));
    TEST_ASSERT_EQUAL(ALARM_VOICE_BAD_ARG, alarm_config_voice_withdraw(&C, 0, 4u));
    TEST_ASSERT_EQUAL(ALARM_VOICE_BAD_ARG, alarm_config_voice_withdraw(&C, ALARM_MAX_ZONES, 4u));
    TEST_ASSERT_EQUAL(ALARM_VOICE_STALE, alarm_config_voice_withdraw(&C, Z_FRONT, 3u));
    TEST_ASSERT_TRUE(alarm_zone_voice_approved(z));
    TEST_ASSERT_EQUAL(ALARM_VOICE_OK, alarm_config_voice_withdraw(&C, Z_FRONT, 4u));
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(z));
    TEST_ASSERT_EQUAL_UINT32(0u, z->voice_pack_revision);
    TEST_ASSERT_EQUAL_UINT32(4u, z->voice_binding_revision);
    TEST_ASSERT_EQUAL_UINT(1, alarm_zone_word_count(z));
    TEST_ASSERT_EQUAL(ALARM_VOICE_OK, alarm_config_voice_withdraw(&C, Z_FRONT, 4u)); /* again */
    TEST_ASSERT_EQUAL(ALARM_VOICE_OK, alarm_config_voice_approve(&C, Z_FRONT, 4u, w, 1u));

    /* A sensor change: a new identity, no approval, the words stay; honoured by a write. */
    TEST_ASSERT_EQUAL(ALARM_ERR_BAD_ARG, alarm_config_voice_invalidate_zone(&C, 0));
    TEST_ASSERT_EQUAL(ALARM_ERR_BAD_ARG, alarm_config_voice_invalidate_zone(&C, ALARM_MAX_ZONES));
    TEST_ASSERT_EQUAL(ALARM_ERR_BAD_ARG, alarm_config_voice_invalidate_zone(NULL, Z_FRONT));
    old = C;
    next = C;
    EXPECT_RESULT(ALARM_OK, alarm_config_voice_invalidate_zone(&next, Z_FRONT));
    alarm_config_voice_reconcile(&old, &next);
    TEST_ASSERT_EQUAL_UINT32(5u, next.zones[Z_FRONT - 1].voice_binding_revision);
    TEST_ASSERT_EQUAL_UINT32(5u, next.opt.voice_revision_counter);
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(&next.zones[Z_FRONT - 1]));
    TEST_ASSERT_EQUAL_UINT(1, alarm_zone_word_count(&next.zones[Z_FRONT - 1]));
    /* the approval shown for the old binding is stale now */
    TEST_ASSERT_EQUAL(ALARM_VOICE_STALE, alarm_config_voice_approve(&next, Z_FRONT, 4u, w, 1u));

    /* The counter's last revision is handed out once; then new identities get none and no
     * approval is possible, while the alarm configuration stays valid. */
    old = next;
    old.opt.voice_revision_counter = UINT32_MAX - 1u;
    next = old;
    snprintf(next.zones[Z_FRONT - 1].name, ALARM_NAME_LEN, "%s", "Porch");
    alarm_config_voice_reconcile(&old, &next);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, next.zones[Z_FRONT - 1].voice_binding_revision);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, next.opt.voice_revision_counter);
    TEST_ASSERT_EQUAL(ALARM_VOICE_UNAVAILABLE, approve_shown(&next, Z_FRONT, 1u));
    old = next;
    snprintf(next.zones[Z_FRONT - 1].name, ALARM_NAME_LEN, "%s", "Front porch");
    alarm_config_voice_reconcile(&old, &next);
    TEST_ASSERT_EQUAL_UINT32(0u, next.zones[Z_FRONT - 1].voice_binding_revision);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, next.opt.voice_revision_counter);
    TEST_ASSERT_EQUAL(ALARM_VOICE_UNAVAILABLE, approve_shown(&next, Z_FRONT, 1u));
    EXPECT_RESULT(ALARM_OK, alarm_config_voice_invalidate_zone(&next, Z_FRONT));
    TEST_ASSERT_EQUAL_UINT32(0u, next.zones[Z_FRONT - 1].voice_binding_revision);
    EXPECT_RESULT(ALARM_OK, alarm_config_validate(&next, NULL, 0));
    /* a forged approval at the exhausted counter isn't accepted either */
    old = next;
    old.zones[Z_FRONT - 1].voice_binding_revision = 9u;
    next = old;
    next.zones[Z_FRONT - 1].voice_approved_revision = 9u;
    next.zones[Z_FRONT - 1].voice_pack_revision = 1u;
    alarm_config_voice_reconcile(&old, &next);
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(&next.zones[Z_FRONT - 1]));
}

/* CORE-CFG-021, D07: a sensor change invalidates its zone's approval as one transaction: copy
 * the running configuration, invalidate the zones, write that copy (alarm_service_voice_
 * invalidate does the three in one alarm-task call). An edit that landed after some earlier
 * read survives it; only the named zones' voice revisions and the counter move. The old way -
 * a copy read in one call, written back in another - reverts that edit. */
static void test_voice_invalidate_zones_keeps_concurrent_edit(void)
{
    static alarm_config_t cfg, stale, scratch, expect;
    static const uint16_t words[2] = {12, 40};
    uint16_t w[ALARM_ZONE_WORDS] = {12, 40};
    uint8_t zones[ALARM_ZONE_BITMAP_BYTES] = {0};
    sim_start(&S, NULL, NULL);
    const alarm_config_t *now = alarm_engine_config(&S.eng);
    TEST_ASSERT_EQUAL(ALARM_ZT_NOT_USED, now->zones[ALARM_MAX_ZONES - 1].type);
    cfg = *now;
    set_words(&cfg.zones[Z_FRONT - 1], words, 2);
    set_words(&cfg.zones[Z_GARAGE - 1], words, 2);
    EXPECT_RESULT(ALARM_OK, alarm_engine_set_config(&S.eng, &cfg, S.now));
    const uint16_t both[2] = {Z_FRONT, Z_GARAGE};
    for (unsigned i = 0; i < 2u; i++) {
        TEST_ASSERT_EQUAL(ALARM_VOICE_OK,
                          alarm_engine_voice_approve(&S.eng, both[i],
                                                     alarm_engine_config_generation(&S.eng),
                                                     now->zones[both[i] - 1].voice_binding_revision,
                                                     w, 3u, S.now));
    }
    stale = *now; /* what a separate "get" would have read first */

    /* another writer's edit, between that read and the write-back */
    cfg = *now;
    cfg.opt.bell_timeout_s = 600;
    snprintf(cfg.zones[Z_WINDOW - 1].name, ALARM_NAME_LEN, "%s", "Bay window");
    EXPECT_RESULT(ALARM_OK, alarm_engine_set_config(&S.eng, &cfg, S.now));
    expect = *now;

    /* the transaction: Z_FRONT and a zone not in use */
    zones[(Z_FRONT - 1) / 8] |= (uint8_t)(1u << ((Z_FRONT - 1) % 8));
    zones[(ALARM_MAX_ZONES - 1) / 8] |= (uint8_t)(1u << ((ALARM_MAX_ZONES - 1) % 8));
    scratch = *now;
    TEST_ASSERT_EQUAL_UINT(1u, alarm_config_voice_invalidate_zones(&scratch, zones));
    EXPECT_RESULT(ALARM_OK, alarm_engine_set_config(&S.eng, &scratch, S.now));
    TEST_ASSERT_EQUAL_UINT16(600, now->opt.bell_timeout_s);
    TEST_ASSERT_EQUAL_STRING("Bay window", now->zones[Z_WINDOW - 1].name);
    TEST_ASSERT_TRUE(alarm_zone_voice_approved(&now->zones[Z_GARAGE - 1]));
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(&now->zones[Z_FRONT - 1]));
    TEST_ASSERT_EQUAL_UINT(2, alarm_zone_word_count(&now->zones[Z_FRONT - 1]));
    TEST_ASSERT_TRUE(now->zones[Z_FRONT - 1].voice_binding_revision !=
                     expect.zones[Z_FRONT - 1].voice_binding_revision);
    /* nothing else moved: the expected copy with only those fields changed is the stored one */
    expect.zones[Z_FRONT - 1].voice_binding_revision =
        now->zones[Z_FRONT - 1].voice_binding_revision;
    expect.zones[Z_FRONT - 1].voice_approved_revision = 0u;
    expect.zones[Z_FRONT - 1].voice_pack_revision = 0u;
    expect.opt.voice_revision_counter = now->opt.voice_revision_counter;
    TEST_ASSERT_EQUAL_MEMORY(&expect, now, sizeof(expect));

    /* failure paths: nothing named, or nothing to work on, changes nothing */
    memset(zones, 0, sizeof(zones));
    scratch = *now;
    TEST_ASSERT_EQUAL_UINT(0u, alarm_config_voice_invalidate_zones(&scratch, zones));
    TEST_ASSERT_EQUAL_MEMORY(now, &scratch, sizeof(scratch));
    TEST_ASSERT_EQUAL_UINT(0u, alarm_config_voice_invalidate_zones(&scratch, NULL));
    TEST_ASSERT_EQUAL_UINT(0u, alarm_config_voice_invalidate_zones(NULL, zones));

    /* the old way: the stale copy, invalidated and written back, reverts the edit */
    zones[(Z_GARAGE - 1) / 8] |= (uint8_t)(1u << ((Z_GARAGE - 1) % 8));
    TEST_ASSERT_EQUAL_UINT(1u, alarm_config_voice_invalidate_zones(&stale, zones));
    EXPECT_RESULT(ALARM_OK, alarm_engine_set_config(&S.eng, &stale, S.now));
    TEST_ASSERT_TRUE(now->opt.bell_timeout_s != 600);
}

/* CORE-CFG-021: every configuration write goes through the identity rules: the engine keeps
 * its own revisions and counter whatever the writer sends, and approval is its own alarm-task
 * transaction against the configuration generation, binding and words the person was shown. */
static void test_set_config_reconciles_voice(void)
{
    static alarm_config_t cfg, approved_a;
    static const uint16_t front[2] = {12, 40};
    uint16_t w[ALARM_ZONE_WORDS] = {12, 40};
    sim_start(&S, NULL, NULL);
    const alarm_config_t *now = alarm_engine_config(&S.eng);
    uint32_t gen = alarm_engine_config_generation(&S.eng);
    cfg = *now;
    set_words(&cfg.zones[Z_FRONT - 1], front, 2);
    cfg.zones[Z_FRONT - 1].voice_approved_revision = 1u; /* forged */
    cfg.zones[Z_FRONT - 1].voice_pack_revision = 1u;
    cfg.opt.voice_revision_counter = 1000u;              /* forged */
    EXPECT_RESULT(ALARM_OK, alarm_engine_set_config(&S.eng, &cfg, S.now));
    TEST_ASSERT_EQUAL_UINT32(1u, now->opt.voice_revision_counter);
    TEST_ASSERT_EQUAL_UINT32(1u, now->zones[Z_FRONT - 1].voice_binding_revision);
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(&now->zones[Z_FRONT - 1]));
    TEST_ASSERT_EQUAL_UINT(2, alarm_zone_word_count(&now->zones[Z_FRONT - 1]));
    TEST_ASSERT_TRUE(alarm_engine_config_generation(&S.eng) != gen);

    /* the old way - approve on a copy, write it back - grants nothing */
    cfg = *now;
    TEST_ASSERT_EQUAL(ALARM_VOICE_OK, approve_shown(&cfg, Z_FRONT, 3u));
    EXPECT_RESULT(ALARM_OK, alarm_engine_set_config(&S.eng, &cfg, S.now));
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(&now->zones[Z_FRONT - 1]));

    /* the transaction: a generation, binding or words other than the stored ones are stale */
    const uint32_t g = alarm_engine_config_generation(&S.eng);
    const unsigned changes = S.n_config_changed;
    TEST_ASSERT_EQUAL(ALARM_VOICE_STALE,
                      alarm_engine_voice_approve(&S.eng, Z_FRONT, gen, 1u, w, 3u, S.now));
    TEST_ASSERT_EQUAL(ALARM_VOICE_STALE,
                      alarm_engine_voice_approve(&S.eng, Z_FRONT, g, 2u, w, 3u, S.now));
    w[1] = 41u;
    TEST_ASSERT_EQUAL(ALARM_VOICE_STALE,
                      alarm_engine_voice_approve(&S.eng, Z_FRONT, g, 1u, w, 3u, S.now));
    w[1] = 40u;
    TEST_ASSERT_EQUAL(ALARM_VOICE_BAD_ARG,
                      alarm_engine_voice_approve(&S.eng, Z_FRONT, g, 1u, NULL, 3u, S.now));
    TEST_ASSERT_EQUAL(ALARM_VOICE_BAD_ARG,
                      alarm_engine_voice_approve(NULL, Z_FRONT, g, 1u, w, 3u, S.now));
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(&now->zones[Z_FRONT - 1]));
    TEST_ASSERT_EQUAL_UINT32(g, alarm_engine_config_generation(&S.eng));
    TEST_ASSERT_EQUAL_UINT(changes, S.n_config_changed);
    TEST_ASSERT_EQUAL(ALARM_VOICE_OK,
                      alarm_engine_voice_approve(&S.eng, Z_FRONT, g, 1u, w, 3u, S.now));
    TEST_ASSERT_TRUE(alarm_zone_voice_approved(&now->zones[Z_FRONT - 1]));
    TEST_ASSERT_EQUAL_UINT32(3u, now->zones[Z_FRONT - 1].voice_pack_revision);
    TEST_ASSERT_EQUAL_UINT(changes + 1u, S.n_config_changed); /* the platform stores it */
    TEST_ASSERT_TRUE(alarm_engine_config_generation(&S.eng) != g);
    /* the same request again: stale (the generation moved on) */
    TEST_ASSERT_EQUAL(ALARM_VOICE_STALE,
                      alarm_engine_voice_approve(&S.eng, Z_FRONT, g, 1u, w, 4u, S.now));
    TEST_ASSERT_EQUAL_UINT32(3u, now->zones[Z_FRONT - 1].voice_pack_revision);
    approved_a = *now;

    /* a later write, approval fields zeroed by a writer that doesn't know them: kept */
    cfg = *now;
    cfg.zones[Z_FRONT - 1].voice_approved_revision = 0u;
    cfg.zones[Z_FRONT - 1].voice_pack_revision = 0u;
    cfg.opt.bell_timeout_s = 600; /* the rest of the configuration still comes from the writer */
    EXPECT_RESULT(ALARM_OK, alarm_engine_set_config(&S.eng, &cfg, S.now));
    TEST_ASSERT_TRUE(alarm_zone_voice_approved(&now->zones[Z_FRONT - 1]));
    TEST_ASSERT_EQUAL_UINT32(3u, now->zones[Z_FRONT - 1].voice_pack_revision);
    TEST_ASSERT_EQUAL_UINT16(600, now->opt.bell_timeout_s);
    TEST_ASSERT_EQUAL_MEMORY(&cfg.users, &now->users, sizeof(cfg.users));
    TEST_ASSERT_EQUAL_MEMORY(&cfg.devices, &now->devices, sizeof(cfg.devices));

    /* recipe A -> B -> A through writes, then the approved A replayed: no approval */
    cfg = *now;
    alarm_zone_set_word(&cfg.zones[Z_FRONT - 1], 2, 12u);
    EXPECT_RESULT(ALARM_OK, alarm_engine_set_config(&S.eng, &cfg, S.now));
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(&now->zones[Z_FRONT - 1]));
    EXPECT_RESULT(ALARM_OK, alarm_engine_set_config(&S.eng, &approved_a, S.now));
    EXPECT_RESULT(ALARM_OK, alarm_engine_set_config(&S.eng, &approved_a, S.now));
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(&now->zones[Z_FRONT - 1]));
    TEST_ASSERT_EQUAL_UINT32(1u, now->zones[Z_FRONT - 1].voice_binding_revision);
    TEST_ASSERT_EQUAL_UINT32(0u, now->zones[Z_FRONT - 1].voice_pack_revision);

    /* withdrawal: the same transaction shape */
    uint32_t g2 = alarm_engine_config_generation(&S.eng);
    TEST_ASSERT_EQUAL(ALARM_VOICE_OK,
                      alarm_engine_voice_approve(&S.eng, Z_FRONT, g2, 1u, w, 5u, S.now));
    TEST_ASSERT_EQUAL(ALARM_VOICE_STALE,
                      alarm_engine_voice_withdraw(&S.eng, Z_FRONT, g2, 1u, S.now));
    g2 = alarm_engine_config_generation(&S.eng);
    TEST_ASSERT_EQUAL(ALARM_VOICE_STALE,
                      alarm_engine_voice_withdraw(&S.eng, Z_FRONT, g2, 2u, S.now));
    TEST_ASSERT_TRUE(alarm_zone_voice_approved(&now->zones[Z_FRONT - 1]));
    TEST_ASSERT_EQUAL(ALARM_VOICE_OK,
                      alarm_engine_voice_withdraw(&S.eng, Z_FRONT, g2, 1u, S.now));
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(&now->zones[Z_FRONT - 1]));

    /* not while a partition is armed, as any configuration change */
    EXPECT_RESULT(ALARM_OK, sim_arm(&S, 1, CODE_USER, ALARM_LEVEL_AWAY, 0));
    g2 = alarm_engine_config_generation(&S.eng);
    TEST_ASSERT_EQUAL(ALARM_VOICE_BUSY,
                      alarm_engine_voice_approve(&S.eng, Z_FRONT, g2, 1u, w, 5u, S.now));
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(&now->zones[Z_FRONT - 1]));
    EXPECT_RESULT(ALARM_OK, sim_disarm(&S, 1, CODE_USER));
    g2 = alarm_engine_config_generation(&S.eng);
    TEST_ASSERT_EQUAL(ALARM_VOICE_OK,
                      alarm_engine_voice_approve(&S.eng, Z_FRONT, g2, 1u, w, 5u, S.now));

    /* a refused write changes nothing */
    g2 = alarm_engine_config_generation(&S.eng);
    cfg = *now;
    snprintf(cfg.zones[Z_FRONT - 1].name, ALARM_NAME_LEN, "%s", "Porch");
    cfg.opt.cancel_window_s = 1; /* invalid */
    EXPECT_RESULT(ALARM_ERR_BAD_ARG, alarm_engine_set_config(&S.eng, &cfg, S.now));
    TEST_ASSERT_TRUE(alarm_zone_voice_approved(&now->zones[Z_FRONT - 1]));
    TEST_ASSERT_EQUAL_UINT32(1u, now->opt.voice_revision_counter);
    TEST_ASSERT_EQUAL_UINT32(g2, alarm_engine_config_generation(&S.eng));

    /* removal through a write: the zone's voice is gone, the counter isn't */
    cfg = *now;
    memset(&cfg.zones[Z_FRONT - 1], 0, sizeof(cfg.zones[0]));
    for (unsigned i = 0; i < ALARM_MAX_ZONES; i++) {
        if (cfg.zones[i].cross_zone == Z_FRONT) {
            cfg.zones[i].cross_zone = 0;
        }
    }
    EXPECT_RESULT(ALARM_OK, alarm_engine_set_config(&S.eng, &cfg, S.now));
    TEST_ASSERT_EQUAL_UINT32(0u, now->zones[Z_FRONT - 1].voice_binding_revision);
    TEST_ASSERT_EQUAL_UINT32(1u, now->opt.voice_revision_counter);
}

/* CORE-CFG-021, CFG-006: a restored configuration - older or newer than the running one, or
 * restored again after a restore cut short - never brings an approval back and never lowers
 * the counter, so no binding revision is handed out twice. */
static void test_voice_restore(void)
{
    static alarm_config_t current, file, restored, again;
    static const uint16_t front[2] = {12, 40};

    /* The running panel: zone 1 renamed since the backup (binding 9, approved), zone 3 the
     * last identity handed out (10). */
    current = C;
    set_words(&current.zones[Z_FRONT - 1], front, 2);
    current.zones[Z_FRONT - 1].voice_binding_revision = 9u;
    current.zones[Z_FRONT - 1].voice_approved_revision = 9u;
    current.zones[Z_FRONT - 1].voice_pack_revision = 2u;
    current.zones[Z_WINDOW - 1].voice_binding_revision = 10u;
    current.opt.voice_revision_counter = 10u;
    /* The older file: zone 1 approved under binding 3, a counter of 3, and a slot not used
     * with leftovers that must not survive. */
    file = C;
    set_words(&file.zones[Z_FRONT - 1], front, 2);
    file.zones[Z_FRONT - 1].voice_binding_revision = 3u;
    file.zones[Z_FRONT - 1].voice_approved_revision = 3u;
    file.zones[Z_FRONT - 1].voice_pack_revision = 7u;
    file.opt.voice_revision_counter = 3u;
    alarm_zone_config_t *unused = &file.zones[ALARM_MAX_ZONES - 1];
    TEST_ASSERT_EQUAL_INT(ALARM_ZT_NOT_USED, unused->type);
    alarm_zone_set_word(unused, 0, 5u);
    unused->voice_binding_revision = 2u;

    unsigned used = 0;
    for (unsigned i = 0; i < ALARM_MAX_ZONES; i++) {
        used += file.zones[i].type != ALARM_ZT_NOT_USED ? 1u : 0u;
    }
    TEST_ASSERT_TRUE(used >= 2u);

    restored = file;
    alarm_config_voice_restore(&current, &restored);
    TEST_ASSERT_EQUAL_UINT32(10u + used, restored.opt.voice_revision_counter);
    uint32_t seen = 10u;
    for (unsigned i = 0; i < ALARM_MAX_ZONES; i++) {
        const alarm_zone_config_t *z = &restored.zones[i];
        TEST_ASSERT_EQUAL_UINT32(0u, z->voice_approved_revision);
        TEST_ASSERT_EQUAL_UINT32(0u, z->voice_pack_revision);
        if (z->type == ALARM_ZT_NOT_USED) {
            TEST_ASSERT_EQUAL_UINT32(0u, z->voice_binding_revision);
            TEST_ASSERT_EQUAL_UINT(0, alarm_zone_word_count(z));
        } else {
            TEST_ASSERT_EQUAL_UINT32(seen + 1u, z->voice_binding_revision); /* fresh, in order */
            seen = z->voice_binding_revision;
        }
    }
    TEST_ASSERT_EQUAL_UINT(2, alarm_zone_word_count(&restored.zones[Z_FRONT - 1])); /* kept */
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(&restored.zones[Z_FRONT - 1]));
    EXPECT_RESULT(ALARM_OK, alarm_config_validate(&restored, NULL, 0));
    /* and the next write to it can't bring the file's approval back */
    again = file;
    again.zones[Z_FRONT - 1].voice_binding_revision = restored.zones[Z_FRONT - 1].voice_binding_revision;
    again.zones[Z_FRONT - 1].voice_approved_revision = restored.zones[Z_FRONT - 1].voice_binding_revision;
    alarm_config_voice_reconcile(&restored, &again);
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(&again.zones[Z_FRONT - 1]));

    /* Interrupted after the configuration was stored: the panel runs the restored one, and the
     * installer restores the same file again - higher revisions still, no approval. */
    again = file;
    alarm_config_voice_restore(&restored, &again);
    TEST_ASSERT_EQUAL_UINT32(10u + 2u * used, again.opt.voice_revision_counter);
    TEST_ASSERT_EQUAL_UINT32(10u + used + 1u, again.zones[Z_FRONT - 1].voice_binding_revision);
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(&again.zones[Z_FRONT - 1]));
    /* Interrupted before it was stored: the running one is unchanged, and the same restore
     * gives the same result. */
    again = file;
    alarm_config_voice_restore(&current, &again);
    TEST_ASSERT_EQUAL_MEMORY(&restored, &again, sizeof(again));

    /* A file whose revisions are higher than the running panel's (made on a busier panel, or
     * forged): the counter starts above every revision in it, approved ones included. */
    again = file;
    again.zones[Z_FRONT - 1].voice_approved_revision = 40u;
    again.zones[Z_GARAGE - 1].voice_binding_revision = 30u;
    alarm_config_voice_restore(&current, &again);
    TEST_ASSERT_EQUAL_UINT32(41u, again.zones[Z_FRONT - 1].voice_binding_revision);
    TEST_ASSERT_EQUAL_UINT32(40u + used, again.opt.voice_revision_counter);

    /* Exhaustion: one more revision, then none; never wrapping; the configuration stays good. */
    current.opt.voice_revision_counter = UINT32_MAX - 1u;
    again = file;
    alarm_config_voice_restore(&current, &again);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, again.zones[Z_FRONT - 1].voice_binding_revision);
    TEST_ASSERT_EQUAL_UINT32(0u, again.zones[Z_GARAGE - 1].voice_binding_revision);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, again.opt.voice_revision_counter);
    EXPECT_RESULT(ALARM_OK, alarm_config_validate(&again, NULL, 0));

    alarm_config_voice_restore(NULL, &again); /* nothing */
    alarm_config_voice_restore(&current, NULL);
}

/* CORE-CFG-011: an idle engine needs no timer. */
static void test_idle_engine_has_no_deadline(void)
{
    sim_start(&S, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT64(ALARM_TIME_NEVER, alarm_engine_next_deadline(&S.eng));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_are_valid);
    RUN_TEST(test_check_codes);
    RUN_TEST(test_cp01_ranges);
    RUN_TEST(test_zone_validation);
    RUN_TEST(test_user_validation);
    RUN_TEST(test_set_config_requires_idle);
    RUN_TEST(test_set_config_resets_changed_zone);
    RUN_TEST(test_resets_state_rule);
    RUN_TEST(test_init_rejects_bad_input);
    RUN_TEST(test_zone_defaults);
    RUN_TEST(test_armed_in_level_table);
    RUN_TEST(test_names);
    RUN_TEST(test_idle_engine_has_no_deadline);
    RUN_TEST(test_zone_words_shape);
    RUN_TEST(test_voice_reconcile_rules);
    RUN_TEST(test_voice_approve_and_exhaustion);
    RUN_TEST(test_set_config_reconciles_voice);
    RUN_TEST(test_voice_invalidate_zones_keeps_concurrent_edit);
    RUN_TEST(test_voice_restore);
    return UNITY_END();
}
