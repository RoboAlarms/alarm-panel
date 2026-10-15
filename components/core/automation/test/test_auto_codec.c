/* The stored record: auto_encode, auto_decode, the frozen fixture and the NVS cost
 * (docs/features/41 AUT-008; AUT-001 limits; AUT-002 layer 3; AUT-015: the option never breaks a
 * record). */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "auto_fixture_v1.h"
#include "auto_fixture_v2.h"
#include "automation/automation.h"
#include "home_control/home_control.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- the NVS cost (AUT-001, AUT-008) ---- */

/* zbz_panel's count, as a constant expression for the _Static_asserts. */
#define NVS_ENTRIES(b) (1u + ((b) + 3999u) / 4000u + ((b) + 31u) / 32u)
#define AUTO_META_LEN 5u /* compat u8 | gen u16 | next_uid u16 */
#define ID_IGNORABLE_MIN 0x80u /* DESIGN.md section 6: ids from here are ignorable */

/* Phase C's two known ignorable fields (bypass 3 bytes, changed_at 6) moved the pin from 509
 * bytes, 18 entries and 291 for the store. */
_Static_assert(AUTO_RECORD_MAX == 518u, "the largest record: DESIGN.md section 6");
_Static_assert(NVS_ENTRIES(AUTO_RECORD_MAX) == 19u, "a record at its largest: 1 + 1 + 17");
_Static_assert(AUTO_MAX * NVS_ENTRIES(AUTO_RECORD_MAX) + NVS_ENTRIES(AUTO_META_LEN) == 307u,
               "the automations store at its largest, with meta");
_Static_assert(AUTO_MAX * NVS_ENTRIES(AUTO_RECORD_MAX) + NVS_ENTRIES(AUTO_META_LEN) <= 310u,
               "the store's budget (of 3,906 usable)");

/* AUT-008: auto_nvs_entries is the same count as the constant one. */
static void test_nvs_entries(void)
{
    TEST_ASSERT_EQUAL_size_t(1u, auto_nvs_entries(0));
    TEST_ASSERT_EQUAL_size_t(3u, auto_nvs_entries(1));
    TEST_ASSERT_EQUAL_size_t(3u, auto_nvs_entries(32));
    TEST_ASSERT_EQUAL_size_t(4u, auto_nvs_entries(33));
    TEST_ASSERT_EQUAL_size_t(1u + 2u + 126u, auto_nvs_entries(4001));
    for (size_t b = 0; b <= 9000u; b++) {
        TEST_ASSERT_EQUAL_size_t(NVS_ENTRIES(b), auto_nvs_entries(b));
    }
    TEST_ASSERT_EQUAL_size_t(19u, auto_nvs_entries(AUTO_RECORD_MAX));
}

/* ---- helpers ---- */

static auto_zone_t s_zones[ALARM_MAX_ZONES];

static auto_env_t env_of(bool arm_supported)
{
    memset(s_zones, 0, sizeof s_zones);
    s_zones[0] = (auto_zone_t){ALARM_ZT_ENTRY_EXIT_1, 1, ALARM_DEVCLASS_DOOR};
    auto_env_t e = {0x01u, arm_supported, s_zones, true, false};
    return e;
}

/* encode(decode(x)) == x */
static void assert_round_trip(const uint8_t *x, size_t len)
{
    auto_rule_t r;
    uint8_t out[AUTO_RECORD_MAX + 16u];
    (void)auto_decode(x, len, &r);
    size_t n = auto_encode(&r, out, sizeof out);
    TEST_ASSERT_EQUAL_size_t(len, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(x, out, len);
}

/* A decode from the end of a buffer, so a sanitizer build sees any read past len. */
static auto_rec_t decode_at_end(const uint8_t *x, size_t len, auto_rule_t *r)
{
    static uint8_t buf[1024];
    TEST_ASSERT_TRUE(len <= sizeof buf);
    uint8_t *at = &buf[sizeof buf - len];
    if (len > 0u) {
        memcpy(at, x, len);
    }
    return auto_decode(at, len, r);
}

/* ---- the fixture (AUT-008) ---- */

/* AUT-008: (a) the porch light reads as stored and passes the save check. */
static void test_fixture_a_porch_light(void)
{
    auto_rule_t r;
    TEST_ASSERT_EQUAL_INT(AUTO_REC_OK, auto_decode(AUTO_FX_A, sizeof AUTO_FX_A, &r));
    TEST_ASSERT_EQUAL_UINT8(AUTO_REC_OK, r.rec);
    TEST_ASSERT_EQUAL_UINT16(1, r.uid);
    TEST_ASSERT_EQUAL_UINT8(0, r.slot);
    TEST_ASSERT_FALSE(r.off);
    TEST_ASSERT_EQUAL_UINT8(1, r.n);
    TEST_ASSERT_EQUAL_UINT8(AUTO_TRIG_ZONE_OPENED, r.when.kind);
    TEST_ASSERT_EQUAL_UINT16(1, r.when.zone);
    TEST_ASSERT_EQUAL_UINT16(18u * 60u + 1u, r.when.win_from);
    TEST_ASSERT_EQUAL_UINT16(6u * 60u + 1u, r.when.win_to);
    TEST_ASSERT_EQUAL_UINT8(AUTO_ACT_DEVICE, r.then[0].kind);
    TEST_ASSERT_EQUAL_UINT8(DSRC_HA, r.then[0].source);
    TEST_ASSERT_EQUAL_UINT8(DEV_LIGHT, r.then[0].dev_kind);
    TEST_ASSERT_EQUAL_UINT8(ACT_ON, r.then[0].dev_action);
    TEST_ASSERT_EQUAL_INT16(-1, r.then[0].value);
    TEST_ASSERT_EQUAL_STRING("light.porch", r.then[0].ref);
    TEST_ASSERT_EQUAL_STRING("Porch light", r.then[0].name);
    TEST_ASSERT_EQUAL_STRING("", r.then[0].mode);
    TEST_ASSERT_EQUAL_UINT16(10, r.then[0].off_after_min);
    TEST_ASSERT_EQUAL_UINT8(0, r.keep_len);
    auto_env_t env = env_of(false);
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
    assert_round_trip(AUTO_FX_A, sizeof AUTO_FX_A);
}

/* AUT-008: the encoder writes (a) from a rule built by hand, byte for byte. */
static void test_fixture_a_from_a_new_rule(void)
{
    auto_rule_t r;
    memset(&r, 0, sizeof r);
    r.uid = 1;
    r.slot = 3; /* the key, not stored */
    r.n = 1;
    r.when.kind = AUTO_TRIG_ZONE_OPENED;
    r.when.zone = 1;
    r.when.win_from = 1081;
    r.when.win_to = 361;
    r.then[0].kind = AUTO_ACT_DEVICE;
    r.then[0].source = DSRC_HA;
    r.then[0].dev_kind = DEV_LIGHT;
    r.then[0].dev_action = ACT_ON;
    r.then[0].value = -1;
    r.then[0].off_after_min = 10;
    strcpy(r.then[0].ref, "light.porch");
    strcpy(r.then[0].name, "Porch light");
    r.then[1].value = 0; /* beyond n: never written */
    uint8_t out[AUTO_RECORD_MAX];
    TEST_ASSERT_EQUAL_size_t(sizeof AUTO_FX_A, auto_encode(&r, out, sizeof out));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(AUTO_FX_A, out, sizeof AUTO_FX_A);
}

/* AUT-008: (b) armed Away -> lock, then a confirmed scene. */
static void test_fixture_b_lock_and_scene(void)
{
    auto_rule_t r;
    TEST_ASSERT_EQUAL_INT(AUTO_REC_OK, auto_decode(AUTO_FX_B, sizeof AUTO_FX_B, &r));
    TEST_ASSERT_EQUAL_UINT16(2, r.uid);
    TEST_ASSERT_EQUAL_UINT8(2, r.n);
    TEST_ASSERT_EQUAL_UINT8(AUTO_TRIG_ARMED, r.when.kind);
    TEST_ASSERT_EQUAL_HEX8(ALARM_LEVEL_BIT(ALARM_LEVEL_AWAY), r.when.levels);
    TEST_ASSERT_EQUAL_UINT8(0, r.when.partitions);
    TEST_ASSERT_EQUAL_UINT8(DEV_LOCK, r.then[0].dev_kind);
    TEST_ASSERT_EQUAL_UINT8(ACT_LOCK, r.then[0].dev_action);
    TEST_ASSERT_EQUAL_STRING("lock.front_door", r.then[0].ref);
    TEST_ASSERT_EQUAL_UINT8(0, r.then[0].confirmed);
    TEST_ASSERT_EQUAL_UINT8(AUTO_ACT_DEVICE, r.then[1].kind);
    TEST_ASSERT_EQUAL_UINT8(DEV_SCENE, r.then[1].dev_kind);
    TEST_ASSERT_EQUAL_UINT8(ACT_RUN, r.then[1].dev_action);
    TEST_ASSERT_EQUAL_INT16(-1, r.then[1].value);
    TEST_ASSERT_EQUAL_STRING("scene.good_night", r.then[1].ref);
    TEST_ASSERT_EQUAL_UINT8(1, r.then[1].confirmed);
    auto_env_t env = env_of(false);
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
    assert_round_trip(AUTO_FX_B, sizeof AUTO_FX_B);
}

/* AUT-008: (c) 23:00 Monday to Friday -> the thermostat off. */
static void test_fixture_c_climate_off(void)
{
    auto_rule_t r;
    TEST_ASSERT_EQUAL_INT(AUTO_REC_OK, auto_decode(AUTO_FX_C, sizeof AUTO_FX_C, &r));
    TEST_ASSERT_EQUAL_UINT16(3, r.uid);
    TEST_ASSERT_EQUAL_UINT8(AUTO_TRIG_TIME, r.when.kind);
    TEST_ASSERT_EQUAL_UINT16(23u * 60u, r.when.minute);
    TEST_ASSERT_EQUAL_HEX8(0x1F, r.when.days);
    TEST_ASSERT_EQUAL_UINT8(DEV_CLIMATE, r.then[0].dev_kind);
    TEST_ASSERT_EQUAL_UINT8(ACT_SET_MODE, r.then[0].dev_action);
    TEST_ASSERT_EQUAL_STRING("off", r.then[0].mode);
    TEST_ASSERT_EQUAL_STRING("climate.hallway", r.then[0].ref);
    auto_env_t env = env_of(false);
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
    assert_round_trip(AUTO_FX_C, sizeof AUTO_FX_C);
}

/* AUT-008: (d) an Arm rule decodes OK; phase A refuses to run it (UNSUPPORTED), phase C runs it. */
static void test_fixture_d_arm_night(void)
{
    auto_rule_t r;
    TEST_ASSERT_EQUAL_INT(AUTO_REC_OK, auto_decode(AUTO_FX_D, sizeof AUTO_FX_D, &r));
    TEST_ASSERT_EQUAL_UINT16(4, r.uid);
    TEST_ASSERT_EQUAL_UINT8(2, r.made_by);
    TEST_ASSERT_EQUAL_UINT8(AUTO_TRIG_TIME, r.when.kind);
    TEST_ASSERT_EQUAL_UINT16(22u * 60u + 30u, r.when.minute);
    TEST_ASSERT_EQUAL_UINT8(0, r.when.days);
    TEST_ASSERT_EQUAL_UINT8(1, r.n);
    TEST_ASSERT_EQUAL_UINT8(AUTO_ACT_ARM, r.then[0].kind);
    TEST_ASSERT_EQUAL_UINT8(1, r.then[0].partition);
    TEST_ASSERT_EQUAL_UINT8(ALARM_LEVEL_NIGHT, r.then[0].arm_level);
    TEST_ASSERT_EQUAL_UINT8(5, r.then[0].warn_min);
    TEST_ASSERT_EQUAL_INT16(-1, r.then[0].value);
    auto_env_t env = env_of(false);
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_UNSUPPORTED, auto_rule_check(&r, &env));
    env.arm_supported = true;
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
    assert_round_trip(AUTO_FX_D, sizeof AUTO_FX_D);
}

/* AUT-008: (e) an unknown critical id and (g) a higher compat are NEWER. */
static void test_fixture_e_and_g_newer(void)
{
    auto_rule_t r;
    TEST_ASSERT_EQUAL_INT(AUTO_REC_NEWER, auto_decode(AUTO_FX_E, sizeof AUTO_FX_E, &r));
    TEST_ASSERT_EQUAL_UINT8(AUTO_REC_NEWER, r.rec);
    TEST_ASSERT_EQUAL_UINT16(5, r.uid); /* still shown */
    TEST_ASSERT_EQUAL_STRING("light.porch", r.then[0].ref);
    TEST_ASSERT_EQUAL_INT(AUTO_REC_NEWER, auto_decode(AUTO_FX_G, sizeof AUTO_FX_G, &r));
    TEST_ASSERT_EQUAL_UINT8(AUTO_REC_NEWER, r.rec);
}

/* AUT-008: (f) an ignorable field is kept and survives an off toggle byte for byte. */
static void test_fixture_f_survives_off_toggle(void)
{
    auto_rule_t r;
    uint8_t off[AUTO_RECORD_MAX];
    uint8_t on[AUTO_RECORD_MAX];
    TEST_ASSERT_EQUAL_INT(AUTO_REC_OK, auto_decode(AUTO_FX_F, sizeof AUTO_FX_F, &r));
    TEST_ASSERT_EQUAL_UINT8(6, r.keep_len);
    static const uint8_t kept[] = {0x81, 0x04, 0xDE, 0xAD, 0xBE, 0xEF};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kept, r.keep, sizeof kept);
    r.off = true;
    size_t n = auto_encode(&r, off, sizeof off);
    TEST_ASSERT_EQUAL_size_t(sizeof AUTO_FX_F + 3u, n); /* flags 0x02 01 01 */
    TEST_ASSERT_EQUAL_INT(AUTO_REC_OK, auto_decode(off, n, &r));
    TEST_ASSERT_TRUE(r.off);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kept, r.keep, sizeof kept);
    r.off = false;
    n = auto_encode(&r, on, sizeof on);
    TEST_ASSERT_EQUAL_size_t(sizeof AUTO_FX_F, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(AUTO_FX_F, on, sizeof AUTO_FX_F);
}

/* AUT-008, AUT-002 (layer 3): (h) a stored script is BROKEN: kept, shown, never run. */
static void test_fixture_h_broken(void)
{
    auto_rule_t r;
    TEST_ASSERT_EQUAL_INT(AUTO_REC_BROKEN, auto_decode(AUTO_FX_H, sizeof AUTO_FX_H, &r));
    TEST_ASSERT_EQUAL_UINT8(AUTO_REC_BROKEN, r.rec);
    TEST_ASSERT_EQUAL_UINT16(8, r.uid);
    TEST_ASSERT_EQUAL_UINT8(DEV_SCRIPT, r.then[0].dev_kind);
    TEST_ASSERT_EQUAL_UINT8(ACT_RUN, r.then[0].dev_action);
    TEST_ASSERT_EQUAL_STRING("script.bedtime", r.then[0].ref);
    auto_env_t env = env_of(false);
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_NOT_ALLOWED, auto_rule_check(&r, &env));
    assert_round_trip(AUTO_FX_H, sizeof AUTO_FX_H); /* kept as stored */
}

/* AUT-008, AUT-002, AUT-015 (D23, D25): (i) a stored Unlock decodes OK - the installer option
 * never makes a record BROKEN; with the option off the save check refuses it, and it is kept
 * byte for byte either way. */
static void test_fixture_i_unlock(void)
{
    auto_rule_t r;
    TEST_ASSERT_EQUAL_INT(AUTO_REC_OK, auto_decode(AUTO_FX_I, sizeof AUTO_FX_I, &r));
    TEST_ASSERT_EQUAL_UINT16(9, r.uid);
    TEST_ASSERT_EQUAL_UINT8(2, r.made_by);
    TEST_ASSERT_EQUAL_UINT8(AUTO_TRIG_DISARMED, r.when.kind);
    TEST_ASSERT_EQUAL_UINT8(DEV_LOCK, r.then[0].dev_kind);
    TEST_ASSERT_EQUAL_UINT8(ACT_UNLOCK, r.then[0].dev_action);
    TEST_ASSERT_EQUAL_STRING("lock.front_door", r.then[0].ref);
    auto_env_t env = env_of(false);
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
    env.less_secure = false;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_LESS_SECURE_OFF, auto_rule_check(&r, &env));
    TEST_ASSERT_EQUAL_INT(AUTO_REC_OK, auto_decode(AUTO_FX_I, sizeof AUTO_FX_I, &r));
    assert_round_trip(AUTO_FX_I, sizeof AUTO_FX_I);
}

/* AUT-008, AUT-011 (D23): (j) a Disarm decodes OK and round-trips; phase A refuses to run it
 * (UNSUPPORTED), phase C runs it, and only with the option on. */
static void test_fixture_j_disarm(void)
{
    auto_rule_t r;
    TEST_ASSERT_EQUAL_INT(AUTO_REC_OK, auto_decode(AUTO_FX_J, sizeof AUTO_FX_J, &r));
    TEST_ASSERT_EQUAL_UINT16(10, r.uid);
    TEST_ASSERT_EQUAL_UINT8(2, r.made_by);
    TEST_ASSERT_EQUAL_UINT8(AUTO_TRIG_TIME, r.when.kind);
    TEST_ASSERT_EQUAL_UINT16(600, r.when.minute);
    TEST_ASSERT_EQUAL_UINT8(0x1F, r.when.days);
    TEST_ASSERT_EQUAL_UINT8(1, r.n);
    TEST_ASSERT_EQUAL_UINT8(AUTO_ACT_DISARM, r.then[0].kind);
    TEST_ASSERT_EQUAL_UINT8(1, r.then[0].partition);
    TEST_ASSERT_EQUAL_UINT8(0, r.then[0].arm_level);
    auto_env_t env = env_of(false);
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_UNSUPPORTED, auto_rule_check(&r, &env));
    env.arm_supported = true;
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
    env.less_secure = false;
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_LESS_SECURE_OFF, auto_rule_check(&r, &env));
    assert_round_trip(AUTO_FX_J, sizeof AUTO_FX_J);
    /* and one this firmware makes, with a confirm, through encode and decode */
    r.then[0].confirmed = 1;
    r.then[0].partition = 2;
    uint8_t out[AUTO_RECORD_MAX];
    const size_t n = auto_encode(&r, out, sizeof out);
    TEST_ASSERT_TRUE(n > 0u);
    auto_rule_t back;
    TEST_ASSERT_EQUAL_INT(AUTO_REC_OK, auto_decode(out, n, &back));
    TEST_ASSERT_EQUAL_UINT8(AUTO_ACT_DISARM, back.then[0].kind);
    TEST_ASSERT_EQUAL_UINT8(2, back.then[0].partition);
    TEST_ASSERT_EQUAL_UINT8(1, back.then[0].confirmed);
    TEST_ASSERT_EQUAL_UINT8(2, back.made_by);
}

/* AUT-008: a record cut at every byte never reads past its end and never invents a field: it is
 * BAD or NEWER, or (cut between two fields) exactly the fields before the cut. */
static void test_truncated_at_every_offset(void)
{
    static const struct {
        const uint8_t *b;
        size_t len;
    } FX[] = {
        {AUTO_FX_A, sizeof AUTO_FX_A}, {AUTO_FX_B, sizeof AUTO_FX_B},
        {AUTO_FX_C, sizeof AUTO_FX_C}, {AUTO_FX_D, sizeof AUTO_FX_D},
        {AUTO_FX_E, sizeof AUTO_FX_E}, {AUTO_FX_F, sizeof AUTO_FX_F},
        {AUTO_FX_G, sizeof AUTO_FX_G}, {AUTO_FX_H, sizeof AUTO_FX_H},
        {AUTO_FX_I, sizeof AUTO_FX_I}, {AUTO_FX_J, sizeof AUTO_FX_J},
    };
    for (size_t f = 0; f < sizeof FX / sizeof FX[0]; f++) {
        for (size_t cut = 0; cut < FX[f].len; cut++) {
            char what[40];
            snprintf(what, sizeof what, "fixture %u cut at %u", (unsigned)f, (unsigned)cut);
            auto_rule_t r;
            auto_rec_t rec = decode_at_end(FX[f].b, cut, &r);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(rec, r.rec, what);
            if (rec == AUTO_REC_BAD || rec == AUTO_REC_NEWER) {
                continue;
            }
            uint8_t out[AUTO_RECORD_MAX];
            size_t n = auto_encode(&r, out, sizeof out);
            TEST_ASSERT_EQUAL_size_t_MESSAGE(cut, n, what);
            TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(FX[f].b, out, cut, what);
        }
    }
    /* cut inside the uid: BAD */
    auto_rule_t r;
    TEST_ASSERT_EQUAL_INT(AUTO_REC_BAD, decode_at_end(AUTO_FX_A, 0, &r));
    TEST_ASSERT_EQUAL_INT(AUTO_REC_BAD, decode_at_end(AUTO_FX_A, 2, &r));
    TEST_ASSERT_EQUAL_INT(AUTO_REC_BAD, decode_at_end(AUTO_FX_A, 4, &r));
    TEST_ASSERT_EQUAL_INT(AUTO_REC_BAD, decode_at_end(AUTO_FX_A, sizeof AUTO_FX_A - 1u, &r));
}

/* ---- fixture v2: phase C's ignorable fields (AUT-008, AUT-014, D29, D30) ---- */

/* (k) "Bypass open zones" and changed_at read back, and the record is written back unchanged. */
static void test_fixture_v2_k_bypass_and_changed_at(void)
{
    auto_rule_t r;
    TEST_ASSERT_EQUAL_INT(AUTO_REC_OK, decode_at_end(AUTO_FX_K, sizeof AUTO_FX_K, &r));
    TEST_ASSERT_EQUAL_UINT16(11u, r.uid);
    TEST_ASSERT_EQUAL_UINT8(AUTO_ACT_ARM, r.then[0].kind);
    TEST_ASSERT_EQUAL_UINT8(1u, r.then[0].bypass_open);
    TEST_ASSERT_EQUAL_UINT32(1800000000u, r.changed_at);
    TEST_ASSERT_EQUAL_UINT8(0u, r.keep_len); /* known: not kept as unknown bytes */
    assert_round_trip(AUTO_FX_K, sizeof AUTO_FX_K);
    /* a new rule like it encodes to the same bytes */
    auto_rule_t n;
    memset(&n, 0, sizeof n);
    n.uid = 11u;
    n.made_by = 2u;
    n.n = 1u;
    n.when.kind = AUTO_TRIG_TIME;
    n.when.minute = 1350u;
    n.then[0].kind = AUTO_ACT_ARM;
    n.then[0].value = -1;
    n.then[0].partition = 1u;
    n.then[0].arm_level = ALARM_LEVEL_NIGHT;
    n.then[0].warn_min = 5u;
    n.then[0].bypass_open = 1u;
    n.changed_at = 1800000000u;
    uint8_t out[AUTO_RECORD_MAX];
    TEST_ASSERT_EQUAL_size_t(sizeof AUTO_FX_K, auto_encode(&n, out, sizeof out));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(AUTO_FX_K, out, sizeof AUTO_FX_K);
    /* every byte cut short is BAD or a shorter rule, never a read past the end */
    for (size_t len = 1; len < sizeof AUTO_FX_K; len++) {
        auto_rule_t cut;
        (void)decode_at_end(AUTO_FX_K, len, &cut);
    }
}

/* (l) An older reader: the fields are ignorable ids it doesn't know - the rule loads OK, arms
 * without bypassing (safe: it fails on an open zone) and keeps the bytes through a rewrite. */
static void test_fixture_v2_l_older_reader(void)
{
    auto_rule_t r;
    TEST_ASSERT_EQUAL_INT(AUTO_REC_OK, decode_at_end(AUTO_FX_L, sizeof AUTO_FX_L, &r));
    TEST_ASSERT_EQUAL_UINT8(0u, r.then[0].bypass_open);
    TEST_ASSERT_EQUAL_UINT32(0u, r.changed_at);
    TEST_ASSERT_EQUAL_UINT8(9u, r.keep_len);
    assert_round_trip(AUTO_FX_L, sizeof AUTO_FX_L);
    /* the same rule as (k) in everything else */
    auto_rule_t k;
    TEST_ASSERT_EQUAL_INT(AUTO_REC_OK, auto_decode(AUTO_FX_K, sizeof AUTO_FX_K, &k));
    k.then[0].bypass_open = 0u;
    TEST_ASSERT_FALSE(auto_rule_sentence_changed(&k, &r));
    TEST_ASSERT_EQUAL_UINT8(ID_IGNORABLE_MIN, (uint8_t)(AUTO_FX_K[sizeof AUTO_FX_K - 9u] & 0x80u));
    TEST_ASSERT_EQUAL_UINT8(ID_IGNORABLE_MIN, (uint8_t)(AUTO_FX_K[sizeof AUTO_FX_K - 6u] & 0x80u));
}

/* (m) A stray bypass bit on a device action is dropped (the rule stays OK and runnable); a
 * changed_at of the wrong length is kept as stored. */
static void test_fixture_v2_m_stray_fields(void)
{
    auto_rule_t r;
    TEST_ASSERT_EQUAL_INT(AUTO_REC_OK, decode_at_end(AUTO_FX_M, sizeof AUTO_FX_M, &r));
    TEST_ASSERT_EQUAL_UINT8(0u, r.then[0].bypass_open);
    TEST_ASSERT_EQUAL_UINT32(0u, r.changed_at);
    TEST_ASSERT_EQUAL_UINT8(4u, r.keep_len);
    static const uint8_t kept[] = {0x83, 0x02, 0x34, 0x12};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kept, r.keep, sizeof kept);
    auto_env_t env = {0x01u, true, NULL, true, false};
    TEST_ASSERT_EQUAL_INT(AUTO_OK, auto_rule_check(&r, &env));
    /* written back without the meaningless bit, the odd field kept */
    uint8_t out[AUTO_RECORD_MAX];
    const size_t n = auto_encode(&r, out, sizeof out);
    TEST_ASSERT_EQUAL_size_t(sizeof AUTO_FX_M - 3u, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kept, &out[n - 4u], 4u);
}

/* ---- the codec's own cases (AUT-008) ---- */

/* A rule with every field set, both actions full, and AUTO_KEEP_MAX ignorable bytes. */
static auto_rule_t worst_rule(void)
{
    auto_rule_t r;
    memset(&r, 0, sizeof r);
    r.uid = 0xFFFF;
    r.off = true;
    r.made_by = ALARM_MAX_USERS;
    r.n = AUTO_ACTIONS;
    r.when = (auto_trigger_t){AUTO_TRIG_ZONE_CLOSED, 0x0F, 0x0E, 0x7F, 128, 1439, 1440, 1};
    for (unsigned i = 0; i < AUTO_ACTIONS; i++) {
        auto_action_t *a = &r.then[i];
        a->kind = AUTO_ACT_DEVICE;
        a->source = DSRC_ZIGBEE;
        a->dev_kind = DEV_SCRIPT;
        a->dev_action = ACT_RUN;
        a->confirmed = 1;
        a->partition = 4;
        a->arm_level = ALARM_LEVEL_NIGHT;
        a->warn_min = 15;
        a->value = (int16_t)(i == 0u ? -32768 : 32767);
        a->off_after_min = 1440;
        memset(a->mode, 'm', DEV_MODE_LEN - 1u);
        memset(a->ref, (int)('a' + i), DEV_REF_LEN - 1u);
        memset(a->name, 'n', DEV_NAME_LEN - 1u);
    }
    /* the known ignorable fields (phase C): "Bypass open zones" is an Arm action's */
    r.then[1].kind = AUTO_ACT_ARM;
    r.then[1].bypass_open = 1;
    r.changed_at = 0xFFFFFFFFu;
    r.keep[0] = 0x80;
    r.keep[1] = (uint8_t)(AUTO_KEEP_MAX - 2u);
    memset(&r.keep[2], 0x5A, AUTO_KEEP_MAX - 2u);
    r.keep_len = AUTO_KEEP_MAX;
    return r;
}

/* AUT-001, AUT-008: the largest record is AUTO_RECORD_MAX bytes and reads back unchanged. */
static void test_worst_case_record(void)
{
    auto_rule_t r = worst_rule();
    uint8_t out[AUTO_RECORD_MAX + 8u];
    size_t n = auto_encode(&r, out, sizeof out);
    TEST_ASSERT_EQUAL_size_t(AUTO_RECORD_MAX, n);
    auto_rule_t back;
    TEST_ASSERT_EQUAL_INT(AUTO_REC_BROKEN, auto_decode(out, n, &back)); /* a script: never run */
    TEST_ASSERT_EQUAL_INT16(-32768, back.then[0].value);
    TEST_ASSERT_EQUAL_INT16(32767, back.then[1].value);
    TEST_ASSERT_EQUAL_UINT8(AUTO_KEEP_MAX, back.keep_len);
    TEST_ASSERT_EQUAL_size_t(DEV_REF_LEN - 1u, strlen(back.then[1].ref));
    assert_round_trip(out, n);
}

/* AUT-008: encode returns 0 when cap is too small and never writes past cap. */
static void test_encode_cap(void)
{
    auto_rule_t r = worst_rule();
    uint8_t out[AUTO_RECORD_MAX + 8u];
    for (size_t cap = 0; cap < AUTO_RECORD_MAX; cap++) {
        memset(out, 0xCC, sizeof out);
        TEST_ASSERT_EQUAL_size_t(0u, auto_encode(&r, out, cap));
        for (size_t i = cap; i < sizeof out; i++) {
            TEST_ASSERT_EQUAL_HEX8(0xCC, out[i]);
        }
    }
    TEST_ASSERT_EQUAL_size_t(AUTO_RECORD_MAX, auto_encode(&r, out, AUTO_RECORD_MAX));
    TEST_ASSERT_EQUAL_size_t(0u, auto_encode(NULL, out, sizeof out));
    r.keep_len = AUTO_KEEP_MAX + 1u;
    TEST_ASSERT_EQUAL_size_t(0u, auto_encode(&r, out, sizeof out));
    r = worst_rule();
    r.n = AUTO_ACTIONS + 1u;
    TEST_ASSERT_EQUAL_size_t(0u, auto_encode(&r, out, sizeof out));
    r = worst_rule();
    memset(r.then[0].ref, 'x', DEV_REF_LEN); /* no NUL */
    TEST_ASSERT_EQUAL_size_t(0u, auto_encode(&r, out, sizeof out));
}

/* AUT-008: an action's value is absent for -1 and written for anything else, 0 included. */
static void test_value_absent_and_zero(void)
{
    static const uint8_t ZERO[] = {0x01, 0x10, 0x01, 0x02, 0x20, 0x01, 0x01, 0x21, 0x01, 0x01,
                                   0x22, 0x01, 0x01, 0x23, 0x01, 0x01, 0x24, 0x02, 0x00, 0x00,
                                   0x26, 0x01, 'x'};
    auto_rule_t r;
    TEST_ASSERT_EQUAL_INT(AUTO_REC_BROKEN, auto_decode(ZERO, sizeof ZERO, &r)); /* on at 0 % */
    TEST_ASSERT_EQUAL_INT16(0, r.then[0].value);
    assert_round_trip(ZERO, sizeof ZERO);
    r.then[0].value = -1;
    uint8_t out[64];
    size_t n = auto_encode(&r, out, sizeof out);
    TEST_ASSERT_EQUAL_size_t(sizeof ZERO - 4u, n);
    TEST_ASSERT_EQUAL_INT(AUTO_REC_OK, auto_decode(out, n, &r));
    TEST_ASSERT_EQUAL_INT16(-1, r.then[0].value);
}

/* AUT-008: ignorable fields are kept in stored order up to AUTO_KEEP_MAX bytes; past it the
 * record is read-only (NEWER). */
static void test_ignorable_fields(void)
{
    uint8_t rec[160];
    size_t n = 0;
    rec[n++] = 0x01;
    rec[n++] = 0x10; rec[n++] = 0x01; rec[n++] = 0x02;
    rec[n++] = 0x80; rec[n++] = 0x03; rec[n++] = 'P'; rec[n++] = 'a'; rec[n++] = 't';
    rec[n++] = 0xFE; rec[n++] = 0x00;
    auto_rule_t r;
    TEST_ASSERT_EQUAL_INT(AUTO_REC_OK, auto_decode(rec, n, &r));
    TEST_ASSERT_EQUAL_UINT8(7, r.keep_len);
    assert_round_trip(rec, n);
    /* 64 bytes exactly: kept */
    rec[n++] = 0x90;
    rec[n++] = (uint8_t)(AUTO_KEEP_MAX - 7u - 2u);
    memset(&rec[n], 0x11, AUTO_KEEP_MAX - 9u);
    n += AUTO_KEEP_MAX - 9u;
    TEST_ASSERT_EQUAL_INT(AUTO_REC_OK, auto_decode(rec, n, &r));
    TEST_ASSERT_EQUAL_UINT8(AUTO_KEEP_MAX, r.keep_len);
    assert_round_trip(rec, n);
    /* one more: read-only, what fitted still kept */
    rec[n++] = 0x81;
    rec[n++] = 0x00;
    TEST_ASSERT_EQUAL_INT(AUTO_REC_NEWER, auto_decode(rec, n, &r));
    TEST_ASSERT_EQUAL_UINT8(AUTO_KEEP_MAX, r.keep_len);
    TEST_ASSERT_EQUAL_UINT8(AUTO_TRIG_DISARMED, r.when.kind);
}

typedef struct {
    const char *what;
    uint8_t b[12];
    size_t len;
    auto_rec_t want;
} raw_case_t;

/* AUT-008: what isn't a record (BAD) and what is a newer one (NEWER). */
static void test_bad_and_newer(void)
{
    static const raw_case_t T[] = {
        {"erased flash", {0xFF, 0x10, 0x01, 0x02}, 4, AUTO_REC_BAD},
        {"compat 0", {0x00, 0x10, 0x01, 0x02}, 4, AUTO_REC_BAD},
        {"compat only", {0x01}, 1, AUTO_REC_OK},
        {"compat 2", {0x02, 0xFF}, 2, AUTO_REC_NEWER},
        {"compat 0xFE", {0xFE}, 1, AUTO_REC_NEWER},
        {"an id without its length", {0x01, 0x10}, 2, AUTO_REC_BAD},
        {"a value cut short", {0x01, 0x01, 0x02, 0x05}, 4, AUTO_REC_BAD},
        {"a u8 of 2 bytes", {0x01, 0x10, 0x02, 0x02, 0x00}, 5, AUTO_REC_BAD},
        {"a u8 of 0 bytes", {0x01, 0x10, 0x00}, 3, AUTO_REC_BAD},
        {"a u16 of 1 byte", {0x01, 0x13, 0x01, 0x01}, 4, AUTO_REC_BAD},
        {"a u16 of 3 bytes", {0x01, 0x01, 0x03, 0x01, 0x00, 0x00}, 6, AUTO_REC_BAD},
        {"a value of 1 byte", {0x01, 0x24, 0x01, 0x00}, 4, AUTO_REC_BAD},
        {"an empty ref", {0x01, 0x26, 0x00}, 3, AUTO_REC_BAD},
        {"a NUL in a ref", {0x01, 0x26, 0x03, 'a', 0x00, 'b'}, 6, AUTO_REC_BAD},
        {"a NUL in a mode", {0x01, 0x35, 0x02, 0x00, 'o'}, 5, AUTO_REC_BAD},
        {"a NUL in a name", {0x01, 0x27, 0x01, 0x00}, 4, AUTO_REC_BAD},
        {"one field twice", {0x01, 0x10, 0x01, 0x02, 0x10, 0x01, 0x02}, 7, AUTO_REC_BAD},
        {"a trigger kind from later", {0x01, 0x10, 0x01, AUTO_TRIG_COUNT}, 4, AUTO_REC_NEWER},
        {"action kind 0", {0x01, 0x20, 0x01, 0x00}, 4, AUTO_REC_NEWER},
        {"action kind 3 (output)", {0x01, 0x30, 0x01, 0x03}, 4, AUTO_REC_NEWER},
        {"action kind 4 (disarm)", {0x01, 0x30, 0x01, 0x04}, 4, AUTO_REC_OK},
        {"action kind 5", {0x01, 0x30, 0x01, 0x05}, 4, AUTO_REC_NEWER},
        {"a device kind from later", {0x01, 0x22, 0x01, DEV_KIND_COUNT}, 4, AUTO_REC_NEWER},
        {"a device action from later", {0x01, 0x23, 0x01, ACT_COUNT}, 4, AUTO_REC_NEWER},
        {"a source from later", {0x01, 0x21, 0x01, AUTO_SOURCE_LAST + 1u}, 4, AUTO_REC_NEWER},
        {"a flag from later", {0x01, 0x02, 0x01, 0x02}, 4, AUTO_REC_NEWER},
        {"the reserved zone bitmap", {0x01, 0x18, 0x01, 0x01}, 4, AUTO_REC_NEWER},
        {"an action sub-id from later", {0x01, 0x2D, 0x01, 0x01}, 4, AUTO_REC_NEWER},
        {"an id 0x40 from later", {0x01, 0x40, 0x00}, 3, AUTO_REC_NEWER},
        {"newer, then cut short", {0x01, 0x7E, 0x00, 0x10}, 4, AUTO_REC_BAD},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        auto_rule_t r;
        TEST_ASSERT_EQUAL_INT_MESSAGE(T[i].want, decode_at_end(T[i].b, T[i].len, &r), T[i].what);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(T[i].want, r.rec, T[i].what);
    }
    auto_rule_t r;
    TEST_ASSERT_EQUAL_INT(AUTO_REC_BAD, auto_decode(NULL, 4, &r));
    TEST_ASSERT_EQUAL_INT(AUTO_REC_BAD, auto_decode(AUTO_FX_A, sizeof AUTO_FX_A, NULL));
}

/* AUT-008: a string at its longest reads; one byte longer is BAD. */
static void test_string_lengths(void)
{
    static const struct {
        uint8_t id;
        size_t max;
    } S[] = {{0x25, DEV_MODE_LEN - 1u}, {0x26, DEV_REF_LEN - 1u}, {0x27, DEV_NAME_LEN - 1u}};
    for (size_t i = 0; i < sizeof S / sizeof S[0]; i++) {
        uint8_t rec[3 + DEV_REF_LEN];
        rec[0] = 0x01;
        rec[1] = S[i].id;
        rec[2] = (uint8_t)S[i].max;
        memset(&rec[3], 'z', S[i].max + 1u);
        auto_rule_t r;
        auto_rec_t got = decode_at_end(rec, 3u + S[i].max, &r);
        TEST_ASSERT_TRUE(got == AUTO_REC_OK || got == AUTO_REC_BROKEN);
        rec[2] = (uint8_t)(S[i].max + 1u);
        TEST_ASSERT_EQUAL_INT(AUTO_REC_BAD, decode_at_end(rec, 4u + S[i].max, &r));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_nvs_entries);
    RUN_TEST(test_fixture_a_porch_light);
    RUN_TEST(test_fixture_a_from_a_new_rule);
    RUN_TEST(test_fixture_b_lock_and_scene);
    RUN_TEST(test_fixture_c_climate_off);
    RUN_TEST(test_fixture_d_arm_night);
    RUN_TEST(test_fixture_e_and_g_newer);
    RUN_TEST(test_fixture_f_survives_off_toggle);
    RUN_TEST(test_fixture_h_broken);
    RUN_TEST(test_fixture_i_unlock);
    RUN_TEST(test_fixture_j_disarm);
    RUN_TEST(test_truncated_at_every_offset);
    RUN_TEST(test_worst_case_record);
    RUN_TEST(test_encode_cap);
    RUN_TEST(test_value_absent_and_zero);
    RUN_TEST(test_ignorable_fields);
    RUN_TEST(test_bad_and_newer);
    RUN_TEST(test_string_lengths);
    RUN_TEST(test_fixture_v2_k_bypass_and_changed_at);
    RUN_TEST(test_fixture_v2_l_older_reader);
    RUN_TEST(test_fixture_v2_m_stray_fields);
    return UNITY_END();
}
