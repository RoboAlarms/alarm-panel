/* The automations' store over a fake key-value port: every load outcome, the edits and their
 * refusals, the uid that is never reused and the NVS reserve (docs/features/41 DESIGN.md
 * section 6, AUT-008; AUT-001 limits; AUT-011 made_by). */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "auto_fixture_v1.h"
#include "automation/auto_store.h"
#include "home_control/home_control.h"

/* ---- the reserve (AUT-008), as zbz_panel's (test_zbz_contract.c) ---- */

_Static_assert(AUTO_NVS_ENTRIES(AUTO_RECORD_MAX) == 19u, "a record at its largest");
_Static_assert(AUTO_NVS_ENTRIES(AUTO_META_LEN) == 3u, "meta");
_Static_assert(AUTO_NVS_ADD_MIN > AUTO_NVS_RESERVE, "adding stops before the reserve");
_Static_assert(AUTO_MAX * AUTO_NVS_ENTRIES(AUTO_RECORD_MAX) + AUTO_NVS_ENTRIES(AUTO_META_LEN) +
                       AUTO_NVS_ADD_MIN < 31u * 126u,
               "the store and the reserve fit NVS's usable entries");

/* ---- the fake key-value port ---- */

#define KV_MAX 24
#define KV_CAP 1024u

typedef struct {
    bool used;
    char key[16];
    uint8_t data[KV_CAP];
    size_t len;
} kv_ent_t;

typedef struct {
    kv_ent_t e[KV_MAX];
    char fail_get[16];   /* this key reads as an error */
    char fail_set[16];   /* this key's writes are refused */
    char fail_erase[16];
    size_t free_entries;
    unsigned sets;
    unsigned erases;
} fake_kv_t;

static fake_kv_t g_kv;
static auto_store_t g_s;

static kv_ent_t *kv_find(const char *key)
{
    for (unsigned i = 0; i < KV_MAX; i++) {
        if (g_kv.e[i].used && strcmp(g_kv.e[i].key, key) == 0) {
            return &g_kv.e[i];
        }
    }
    return NULL;
}

static void kv_put(const char *key, const uint8_t *data, size_t len)
{
    kv_ent_t *e = kv_find(key);
    for (unsigned i = 0; e == NULL && i < KV_MAX; i++) {
        if (!g_kv.e[i].used) {
            e = &g_kv.e[i];
        }
    }
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(len <= KV_CAP);
    e->used = true;
    snprintf(e->key, sizeof e->key, "%s", key);
    memcpy(e->data, data, len);
    e->len = len;
}

static auto_kv_t fk_get(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    (void)ctx;
    if (strcmp(key, g_kv.fail_get) == 0) {
        return AUTO_KV_ERROR;
    }
    const kv_ent_t *e = kv_find(key);
    if (e == NULL) {
        return AUTO_KV_ABSENT;
    }
    if (e->len > cap) {
        return AUTO_KV_TOO_BIG;
    }
    memcpy(buf, e->data, e->len);
    *len = e->len;
    return AUTO_KV_OK;
}

static bool fk_set(void *ctx, const char *key, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (strcmp(key, g_kv.fail_set) == 0) {
        return false;
    }
    g_kv.sets++;
    kv_put(key, data, len);
    return true;
}

static bool fk_erase(void *ctx, const char *key)
{
    (void)ctx;
    if (strcmp(key, g_kv.fail_erase) == 0) {
        return false;
    }
    g_kv.erases++;
    kv_ent_t *e = kv_find(key);
    if (e != NULL) {
        e->used = false;
    }
    return true;
}

static size_t fk_free(void *ctx)
{
    (void)ctx;
    return g_kv.free_entries;
}

static const auto_kv_port_t KV = {NULL, fk_get, fk_set, fk_erase, fk_free};

static void put_meta(uint8_t compat, uint16_t gen, uint16_t next_uid)
{
    const uint8_t m[5] = {compat, (uint8_t)(gen & 0xFFu), (uint8_t)(gen >> 8),
                          (uint8_t)(next_uid & 0xFFu), (uint8_t)(next_uid >> 8)};
    kv_put("meta", m, sizeof m);
}

static const kv_ent_t *meta_of(uint16_t *gen, uint16_t *next_uid)
{
    const kv_ent_t *e = kv_find("meta");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_size_t(5u, e->len);
    TEST_ASSERT_EQUAL_UINT8(1u, e->data[0]);
    *gen = (uint16_t)((unsigned)e->data[1] | ((unsigned)e->data[2] << 8));
    *next_uid = (uint16_t)((unsigned)e->data[3] | ((unsigned)e->data[4] << 8));
    return e;
}

void setUp(void)
{
    memset(&g_kv, 0, sizeof g_kv);
    g_kv.free_entries = AUTO_NVS_NOT_TOLD;
    memset(&g_s, 0, sizeof g_s);
}
void tearDown(void) {}

/* ---- rules ---- */

static auto_zone_t s_zones[ALARM_MAX_ZONES];

static auto_env_t env_of(bool arm_supported)
{
    memset(s_zones, 0, sizeof s_zones);
    s_zones[0] = (auto_zone_t){ALARM_ZT_ENTRY_EXIT_1, 1, ALARM_DEVCLASS_DOOR};
    auto_env_t e = {0x01u, arm_supported, s_zones, true, false};
    return e;
}

static auto_rule_t porch(const char *ref)
{
    auto_rule_t r;
    memset(&r, 0, sizeof r);
    r.n = 1;
    r.when.kind = AUTO_TRIG_ZONE_OPENED;
    r.when.zone = 1;
    r.then[0].kind = AUTO_ACT_DEVICE;
    r.then[0].source = DSRC_HA;
    r.then[0].dev_kind = DEV_LIGHT;
    r.then[0].dev_action = ACT_ON;
    r.then[0].value = -1;
    snprintf(r.then[0].ref, sizeof r.then[0].ref, "%s", ref);
    return r;
}

static auto_rule_t arm_night(void)
{
    auto_rule_t r;
    memset(&r, 0, sizeof r);
    r.n = 1;
    r.when.kind = AUTO_TRIG_TIME;
    r.when.minute = 1350;
    r.then[0].kind = AUTO_ACT_ARM;
    r.then[0].value = -1;
    r.then[0].partition = 1;
    r.then[0].arm_level = ALARM_LEVEL_NIGHT;
    r.then[0].warn_min = 5;
    return r;
}

static uint16_t add(const auto_rule_t *r)
{
    const auto_env_t env = env_of(false);
    uint16_t uid = 0;
    auto_check_t why = AUTO_ERR_INCOMPLETE;
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK,
                          auto_store_save(&g_s, &KV, r, &env, 1u, g_s.gen, &why, &uid));
    TEST_ASSERT_EQUAL_INT(AUTO_OK, why);
    TEST_ASSERT_NOT_EQUAL(0u, uid);
    return uid;
}

static size_t count(void)
{
    static auto_rule_t out[AUTO_MAX];
    return auto_store_list(&g_s, out, AUTO_MAX);
}

static void load_fixture(void)
{
    kv_put("a01", AUTO_FX_A, sizeof AUTO_FX_A);
    kv_put("a02", AUTO_FX_B, sizeof AUTO_FX_B);
    kv_put("a03", AUTO_FX_C, sizeof AUTO_FX_C);
    kv_put("a04", AUTO_FX_D, sizeof AUTO_FX_D);
    kv_put("a05", AUTO_FX_E, sizeof AUTO_FX_E);
    kv_put("a06", AUTO_FX_F, sizeof AUTO_FX_F);
    kv_put("a07", AUTO_FX_G, sizeof AUTO_FX_G);
    kv_put("a08", AUTO_FX_H, sizeof AUTO_FX_H);
}

/* ---- load outcomes (AUT-008) ---- */

/* AUT-008: nothing stored - an empty, editable store; uids start at 1. */
static void test_load_empty(void)
{
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV));
    TEST_ASSERT_FALSE(g_s.read_only);
    TEST_ASSERT_EQUAL_size_t(0u, count());
    TEST_ASSERT_EQUAL_UINT16(0u, g_s.gen);
    TEST_ASSERT_EQUAL_UINT16(1u, g_s.next_uid);
    TEST_ASSERT_EQUAL_UINT(0u, g_kv.sets); /* loading never writes */
}

/* AUT-008: the frozen fixture's records through the store - OK, NEWER and BROKEN kept and shown,
 * in their slots, with meta's gen and next_uid. */
static void test_load_fixture(void)
{
    load_fixture();
    put_meta(1u, 7u, 9u);
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV));
    TEST_ASSERT_EQUAL_UINT16(7u, g_s.gen);
    TEST_ASSERT_EQUAL_UINT16(9u, g_s.next_uid);
    static const uint8_t want[8] = {AUTO_REC_OK,    AUTO_REC_OK, AUTO_REC_OK,    AUTO_REC_OK,
                                    AUTO_REC_NEWER, AUTO_REC_OK, AUTO_REC_NEWER, AUTO_REC_BROKEN};
    auto_rule_t out[AUTO_MAX];
    TEST_ASSERT_EQUAL_size_t(8u, auto_store_list(&g_s, out, AUTO_MAX));
    for (unsigned i = 0; i < 8u; i++) {
        TEST_ASSERT_EQUAL_UINT8(i + 1u, out[i].slot);
        TEST_ASSERT_EQUAL_UINT8(want[i], out[i].rec);
        TEST_ASSERT_EQUAL_UINT16(0u, g_s.bad_len[i]);
    }
    auto_rule_t r;
    TEST_ASSERT_TRUE(auto_store_get(&g_s, 1u, &r));
    TEST_ASSERT_EQUAL_STRING("light.porch", r.then[0].ref);
    TEST_ASSERT_TRUE(auto_store_get(&g_s, 4u, &r));
    TEST_ASSERT_EQUAL_UINT8(2u, r.made_by);
    TEST_ASSERT_EQUAL_UINT8(4u, r.slot);
    TEST_ASSERT_TRUE(auto_store_get(&g_s, 8u, &r));
    TEST_ASSERT_EQUAL_UINT8(AUTO_REC_BROKEN, r.rec);
    TEST_ASSERT_FALSE(auto_store_get(&g_s, 0u, &r));
    TEST_ASSERT_FALSE(auto_store_get(&g_s, 99u, &r));
}

/* AUT-008: an undecodable record is kept as stored and never overwritten - a save of another
 * rule goes to another slot and leaves its bytes alone; only delete removes it. */
static void test_bad_record_kept(void)
{
    static const uint8_t cut[] = {0x01, 0x01, 0x05, 0x01}; /* uid's value cut short */
    static const uint8_t erased[] = {0xFF, 0xFF, 0xFF};
    kv_put("a01", cut, sizeof cut);
    kv_put("a03", erased, sizeof erased);
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV));
    TEST_ASSERT_EQUAL_UINT8(AUTO_REC_BAD, g_s.rules[0].rec);
    TEST_ASSERT_EQUAL_UINT8(AUTO_REC_BAD, g_s.rules[2].rec);
    TEST_ASSERT_EQUAL_UINT16(sizeof cut, g_s.bad_len[0]);
    TEST_ASSERT_EQUAL_MEMORY(cut, g_s.bad[0], sizeof cut);
    TEST_ASSERT_EQUAL_UINT16(sizeof erased, g_s.bad_len[2]);

    auto_rule_t p = porch("light.porch");
    TEST_ASSERT_EQUAL_UINT16(1u, add(&p));
    TEST_ASSERT_EQUAL_UINT8(2u, g_s.rules[1].slot); /* the first empty slot */
    p = porch("light.hall");
    (void)add(&p);
    TEST_ASSERT_TRUE(g_s.used[3]);
    const kv_ent_t *e = kv_find("a01");
    TEST_ASSERT_EQUAL_size_t(sizeof cut, e->len);
    TEST_ASSERT_EQUAL_MEMORY(cut, e->data, sizeof cut);
    e = kv_find("a03");
    TEST_ASSERT_EQUAL_MEMORY(erased, e->data, sizeof erased);
    TEST_ASSERT_EQUAL_MEMORY(cut, g_s.bad[0], sizeof cut);

    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_delete_slot(&g_s, &KV, 1u, g_s.gen));
    TEST_ASSERT_NULL(kv_find("a01"));
    TEST_ASSERT_FALSE(g_s.used[0]);
    TEST_ASSERT_EQUAL_UINT16(0u, g_s.bad_len[0]);
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_NOT_FOUND, auto_store_delete_slot(&g_s, &KV, 1u, g_s.gen));
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_NOT_FOUND, auto_store_delete_slot(&g_s, &KV, 0u, g_s.gen));
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_NOT_FOUND, auto_store_delete_slot(&g_s, &KV, 17u, g_s.gen));
}

/* AUT-008: NEWER and BROKEN records can't be edited or turned off - only deleted. */
static void test_not_editable(void)
{
    load_fixture();
    put_meta(1u, 0u, 9u);
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV));
    const auto_env_t env = env_of(false);
    auto_rule_t r;
    TEST_ASSERT_TRUE(auto_store_get(&g_s, 5u, &r)); /* (e) NEWER */
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_NOT_EDITABLE,
                          auto_store_save(&g_s, &KV, &r, &env, 1u, 0u, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_NOT_EDITABLE, auto_store_set_off(&g_s, &KV, 5u, true, 0u));
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_NOT_EDITABLE, auto_store_set_off(&g_s, &KV, 8u, true, 0u));
    r = porch("lock.front_door");
    r.uid = 8u; /* (h) BROKEN, even with a valid replacement */
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_NOT_EDITABLE,
                          auto_store_save(&g_s, &KV, &r, &env, 1u, 0u, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT(0u, g_kv.sets);
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_delete(&g_s, &KV, 8u, 0u));
    TEST_ASSERT_NULL(kv_find("a08"));
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_delete_slot(&g_s, &KV, 7u, 1u)); /* (g) */
    TEST_ASSERT_EQUAL_size_t(6u, count());
}

/* AUT-008: a read error other than "absent" makes the whole store read-only; the readable records
 * are still shown, and every edit is refused without a write. */
static void test_read_error_read_only(void)
{
    load_fixture();
    put_meta(1u, 3u, 9u);
    snprintf(g_kv.fail_get, sizeof g_kv.fail_get, "a05");
    TEST_ASSERT_FALSE(auto_store_load(&g_s, &KV));
    TEST_ASSERT_TRUE(g_s.read_only);
    TEST_ASSERT_EQUAL_size_t(7u, count());
    TEST_ASSERT_FALSE(g_s.used[4]);
    const auto_env_t env = env_of(false);
    auto_rule_t p = porch("light.porch");
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_READ_ONLY,
                          auto_store_save(&g_s, &KV, &p, &env, 1u, 3u, NULL, NULL));
    p.uid = 1u;
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_READ_ONLY,
                          auto_store_save(&g_s, &KV, &p, &env, 1u, 3u, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_READ_ONLY, auto_store_delete(&g_s, &KV, 1u, 3u));
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_READ_ONLY, auto_store_delete_slot(&g_s, &KV, 1u, 3u));
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_READ_ONLY, auto_store_set_off(&g_s, &KV, 1u, true, 3u));
    TEST_ASSERT_EQUAL_UINT(0u, g_kv.sets);
    TEST_ASSERT_EQUAL_UINT(0u, g_kv.erases);
}

/* AUT-008: a meta this firmware can't read - unreadable, a higher compat, too short, erased -
 * makes the store read-only; a longer one of compat 1 reads (newer firmware appended to it). */
static void test_meta_outcomes(void)
{
    kv_put("a01", AUTO_FX_A, sizeof AUTO_FX_A);
    snprintf(g_kv.fail_get, sizeof g_kv.fail_get, "meta");
    TEST_ASSERT_FALSE(auto_store_load(&g_s, &KV));
    TEST_ASSERT_EQUAL_size_t(1u, count()); /* still shown */

    g_kv.fail_get[0] = '\0';
    put_meta(2u, 1u, 5u);
    TEST_ASSERT_FALSE(auto_store_load(&g_s, &KV));

    static const uint8_t short_meta[] = {0x01, 0x01, 0x00, 0x05};
    kv_put("meta", short_meta, sizeof short_meta);
    TEST_ASSERT_FALSE(auto_store_load(&g_s, &KV));

    static const uint8_t erased_meta[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    kv_put("meta", erased_meta, sizeof erased_meta);
    TEST_ASSERT_FALSE(auto_store_load(&g_s, &KV));

    static const uint8_t long_meta[] = {0x01, 0x04, 0x00, 0x06, 0x00, 0xAA, 0xBB};
    kv_put("meta", long_meta, sizeof long_meta);
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV));
    TEST_ASSERT_EQUAL_UINT16(4u, g_s.gen);
    TEST_ASSERT_EQUAL_UINT16(6u, g_s.next_uid);
}

/* AUT-008: records without meta - next_uid rebuilt as max(uid) + 1, gen 0; a meta behind a
 * stored uid (damaged, or restored behind its records) never hands that uid out again. */
static void test_meta_rebuilt(void)
{
    kv_put("a02", AUTO_FX_B, sizeof AUTO_FX_B); /* uid 2 */
    kv_put("a05", AUTO_FX_C, sizeof AUTO_FX_C); /* uid 3 */
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV));
    TEST_ASSERT_EQUAL_UINT16(0u, g_s.gen);
    TEST_ASSERT_EQUAL_UINT16(4u, g_s.next_uid);
    TEST_ASSERT_NULL(kv_find("meta")); /* stored with the next change, not by loading */
    auto_rule_t p = porch("light.porch");
    TEST_ASSERT_EQUAL_UINT16(4u, add(&p));
    TEST_ASSERT_EQUAL_UINT8(1u, g_s.rules[0].slot);

    setUp();
    kv_put("a01", AUTO_FX_C, sizeof AUTO_FX_C); /* uid 3 */
    put_meta(1u, 9u, 2u);
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV));
    TEST_ASSERT_EQUAL_UINT16(9u, g_s.gen);
    TEST_ASSERT_EQUAL_UINT16(4u, g_s.next_uid);
}

/* AUT-008: a record longer than any this firmware writes is newer software's: shown read-only,
 * the rest of the store editable. */
static void test_too_big_is_newer(void)
{
    static uint8_t big[AUTO_RECORD_MAX + 40u];
    memset(big, 0x01, sizeof big);
    kv_put("a02", big, sizeof big);
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV));
    TEST_ASSERT_TRUE(g_s.used[1]);
    TEST_ASSERT_EQUAL_UINT8(AUTO_REC_NEWER, g_s.rules[1].rec);
    TEST_ASSERT_EQUAL_UINT8(2u, g_s.rules[1].slot);
    auto_rule_t p = porch("light.porch");
    (void)add(&p);
    TEST_ASSERT_EQUAL_UINT8(1u, g_s.rules[0].slot);
    TEST_ASSERT_EQUAL_size_t(sizeof big, kv_find("a02")->len);
}

/* ---- edits (AUT-008) ---- */

/* AUT-008: add, change, off and delete each write meta with gen + 1, then their record; what is
 * stored loads back the same. */
static void test_round_trips(void)
{
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV));
    auto_rule_t p = porch("light.porch");
    p.off = true;
    const uint16_t uid = add(&p);
    TEST_ASSERT_EQUAL_UINT16(1u, uid);
    uint16_t gen = 0, next = 0;
    (void)meta_of(&gen, &next);
    TEST_ASSERT_EQUAL_UINT16(1u, gen);
    TEST_ASSERT_EQUAL_UINT16(2u, next);
    TEST_ASSERT_EQUAL_UINT16(1u, g_s.gen);
    auto_rule_t got;
    TEST_ASSERT_TRUE(auto_store_get(&g_s, 1u, &got));
    TEST_ASSERT_TRUE(got.off);
    TEST_ASSERT_EQUAL_UINT8(1u, got.slot);

    /* the record as stored decodes to the same rule */
    const kv_ent_t *e = kv_find("a01");
    TEST_ASSERT_NOT_NULL(e);
    auto_rule_t dec;
    TEST_ASSERT_EQUAL_INT(AUTO_REC_OK, auto_decode(e->data, e->len, &dec));
    TEST_ASSERT_EQUAL_UINT16(1u, dec.uid);
    TEST_ASSERT_TRUE(dec.off);
    TEST_ASSERT_EQUAL_STRING("light.porch", dec.then[0].ref);

    /* a change of the same uid */
    got.then[0].off_after_min = 10u;
    const auto_env_t env = env_of(false);
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_save(&g_s, &KV, &got, &env, 1u, 1u, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT16(2u, g_s.gen);
    TEST_ASSERT_EQUAL_UINT16(2u, g_s.next_uid); /* a change allocates nothing */

    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_set_off(&g_s, &KV, 1u, false, 2u));
    TEST_ASSERT_EQUAL_UINT16(3u, g_s.gen);
    const unsigned sets = g_kv.sets;
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_set_off(&g_s, &KV, 1u, false, 3u));
    TEST_ASSERT_EQUAL_UINT(sets, g_kv.sets); /* no change, no write (wear) */
    TEST_ASSERT_EQUAL_UINT16(3u, g_s.gen);

    /* a restart reads what was written */
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV));
    TEST_ASSERT_EQUAL_UINT16(3u, g_s.gen);
    TEST_ASSERT_TRUE(auto_store_get(&g_s, 1u, &got));
    TEST_ASSERT_FALSE(got.off);
    TEST_ASSERT_EQUAL_UINT16(10u, got.then[0].off_after_min);

    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_delete(&g_s, &KV, 1u, 3u));
    TEST_ASSERT_NULL(kv_find("a01"));
    (void)meta_of(&gen, &next);
    TEST_ASSERT_EQUAL_UINT16(4u, gen);
    TEST_ASSERT_EQUAL_UINT16(2u, next);
    TEST_ASSERT_EQUAL_size_t(0u, count());
}

/* AUT-008: a uid is never reused - not after a delete, not after a restart. */
static void test_uid_never_reused(void)
{
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV));
    auto_rule_t p = porch("light.a");
    TEST_ASSERT_EQUAL_UINT16(1u, add(&p));
    p = porch("light.b");
    TEST_ASSERT_EQUAL_UINT16(2u, add(&p));
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_delete(&g_s, &KV, 2u, g_s.gen));
    p = porch("light.c");
    TEST_ASSERT_EQUAL_UINT16(3u, add(&p));
    TEST_ASSERT_EQUAL_UINT8(2u, g_s.rules[1].slot); /* the freed slot, a new uid */
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_delete(&g_s, &KV, 3u, g_s.gen));
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV));
    p = porch("light.d");
    TEST_ASSERT_EQUAL_UINT16(4u, add(&p));
}

/* AUT-008: an edit from a stale page (its gen isn't the store's) is CHANGED, nothing written. */
static void test_changed_on_stale_gen(void)
{
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV));
    auto_rule_t p = porch("light.a");
    (void)add(&p);
    const auto_env_t env = env_of(false);
    const unsigned sets = g_kv.sets;
    p = porch("light.b");
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_CHANGED,
                          auto_store_save(&g_s, &KV, &p, &env, 1u, 0u, NULL, NULL));
    p.uid = 1u;
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_CHANGED,
                          auto_store_save(&g_s, &KV, &p, &env, 1u, 0u, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_CHANGED, auto_store_set_off(&g_s, &KV, 1u, true, 0u));
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_CHANGED, auto_store_delete(&g_s, &KV, 1u, 0u));
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_CHANGED, auto_store_delete_slot(&g_s, &KV, 1u, 2u));
    TEST_ASSERT_EQUAL_UINT(sets, g_kv.sets);
    TEST_ASSERT_EQUAL_UINT(0u, g_kv.erases);
}

/* AUT-001, AUT-008: 16 automations at most; and once every uid has been handed out, no more. */
static void test_full(void)
{
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV));
    for (unsigned i = 0; i < AUTO_MAX; i++) {
        char ref[16];
        snprintf(ref, sizeof ref, "light.l%u", i);
        auto_rule_t p = porch(ref);
        (void)add(&p);
    }
    const auto_env_t env = env_of(false);
    auto_rule_t p = porch("light.more");
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_FULL,
                          auto_store_save(&g_s, &KV, &p, &env, 1u, g_s.gen, NULL, NULL));
    TEST_ASSERT_NULL(kv_find("a17"));
    p.uid = 16u; /* a change still goes through */
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK,
                          auto_store_save(&g_s, &KV, &p, &env, 1u, g_s.gen, NULL, NULL));

    setUp();
    put_meta(1u, 0u, 0xFFFFu);
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV));
    p = porch("light.last");
    TEST_ASSERT_EQUAL_UINT16(0xFFFFu, add(&p));
    TEST_ASSERT_EQUAL_UINT16(0u, g_s.next_uid);
    p = porch("light.none");
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_FULL,
                          auto_store_save(&g_s, &KV, &p, &env, 1u, g_s.gen, NULL, NULL));
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV)); /* and after a restart */
    TEST_ASSERT_EQUAL_UINT16(0u, g_s.next_uid);
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_FULL,
                          auto_store_save(&g_s, &KV, &p, &env, 1u, g_s.gen, NULL, NULL));
}

/* AUT-008: under AUTO_NVS_ADD_MIN free entries an add is refused NO_ROOM; a change, an off and a
 * delete still write; at AUTO_NVS_ADD_MIN, or unknown, adding works again. */
static void test_no_room_only_for_adds(void)
{
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV));
    auto_rule_t p = porch("light.a");
    (void)add(&p);
    g_kv.free_entries = AUTO_NVS_ADD_MIN - 1u;
    const auto_env_t env = env_of(false);
    p = porch("light.b");
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_NO_ROOM,
                          auto_store_save(&g_s, &KV, &p, &env, 1u, g_s.gen, NULL, NULL));
    TEST_ASSERT_EQUAL_size_t(1u, count());
    TEST_ASSERT_EQUAL_UINT16(2u, g_s.next_uid);
    p.uid = 1u;
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK,
                          auto_store_save(&g_s, &KV, &p, &env, 1u, g_s.gen, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_set_off(&g_s, &KV, 1u, true, g_s.gen));
    g_kv.free_entries = AUTO_NVS_ADD_MIN;
    p = porch("light.c");
    TEST_ASSERT_EQUAL_UINT16(2u, add(&p));
    g_kv.free_entries = 0u;
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_delete(&g_s, &KV, 2u, g_s.gen));
    g_kv.free_entries = AUTO_NVS_NOT_TOLD;
    p = porch("light.d");
    TEST_ASSERT_EQUAL_UINT16(3u, add(&p));
}

/* AUT-002, AUT-008: every save goes through the validator; a refused one writes nothing and says
 * why. Unknown uids are NOT_FOUND. */
static void test_invalid_and_not_found(void)
{
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV));
    const auto_env_t env = env_of(false);
    auto_rule_t p = porch("script.x");
    p.then[0].dev_kind = DEV_SCRIPT;
    p.then[0].dev_action = ACT_RUN;
    auto_check_t why = AUTO_OK;
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_INVALID,
                          auto_store_save(&g_s, &KV, &p, &env, 1u, 0u, &why, NULL));
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_NOT_ALLOWED, why);
    p = porch("light.porch");
    p.when.zone = 2; /* not in use */
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_INVALID,
                          auto_store_save(&g_s, &KV, &p, &env, 1u, 0u, &why, NULL));
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_ZONE, why);
    TEST_ASSERT_EQUAL_UINT(0u, g_kv.sets);
    TEST_ASSERT_EQUAL_UINT16(0u, g_s.gen);
    TEST_ASSERT_EQUAL_UINT16(1u, g_s.next_uid);

    p = porch("light.porch");
    p.uid = 7u;
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_NOT_FOUND,
                          auto_store_save(&g_s, &KV, &p, &env, 1u, 0u, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_NOT_FOUND, auto_store_delete(&g_s, &KV, 7u, 0u));
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_NOT_FOUND, auto_store_delete(&g_s, &KV, 0u, 0u));
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_NOT_FOUND, auto_store_set_off(&g_s, &KV, 7u, true, 0u));
}

/* AUT-011, AUT-015, AUT-008 (D23): every save stores the saver as the maker - never the
 * request's number - and an edit makes the editor the maker; no saver, no Arm rule and no less
 * secure one, while a light needs none. */
static void test_made_by_is_the_saver(void)
{
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV));
    const auto_env_t env = env_of(true);
    auto_rule_t r = arm_night();
    r.made_by = 9u;
    uint16_t uid = 0;
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_save(&g_s, &KV, &r, &env, 3u, 0u, NULL, &uid));
    auto_rule_t got;
    TEST_ASSERT_TRUE(auto_store_get(&g_s, uid, &got));
    TEST_ASSERT_EQUAL_UINT8(3u, got.made_by);
    auto_rule_t dec;
    const kv_ent_t *e = kv_find("a01");
    TEST_ASSERT_EQUAL_INT(AUTO_REC_OK, auto_decode(e->data, e->len, &dec));
    TEST_ASSERT_EQUAL_UINT8(3u, dec.made_by);

    got.made_by = 1u; /* a change by another user acts as that user */
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_save(&g_s, &KV, &got, &env, 5u, 1u, NULL, NULL));
    TEST_ASSERT_TRUE(auto_store_get(&g_s, uid, &got));
    TEST_ASSERT_EQUAL_UINT8(5u, got.made_by);

    auto_rule_t p = porch("light.porch");
    p.made_by = 4u;
    uint16_t light = 0;
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK,
                          auto_store_save(&g_s, &KV, &p, &env, 3u, 2u, NULL, &light));
    TEST_ASSERT_TRUE(auto_store_get(&g_s, light, &got));
    TEST_ASSERT_EQUAL_UINT8(3u, got.made_by); /* a device rule keeps its maker too */
    got.made_by = 0u;
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_save(&g_s, &KV, &got, &env, 6u, 3u, NULL, NULL));
    TEST_ASSERT_TRUE(auto_store_get(&g_s, light, &got));
    TEST_ASSERT_EQUAL_UINT8(6u, got.made_by); /* the editor */

    r = arm_night();
    r.made_by = 2u;
    auto_check_t why = AUTO_OK;
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_INVALID,
                          auto_store_save(&g_s, &KV, &r, &env, 0u, 4u, &why, NULL));
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_MAKER, why);

    /* an unlock (at a time of day: no confirm) with no saver */
    p = porch("lock.front");
    p.when.kind = AUTO_TRIG_TIME;
    p.when.zone = 0;
    p.when.minute = 600;
    p.then[0].dev_kind = DEV_LOCK;
    p.then[0].dev_action = ACT_UNLOCK;
    p.made_by = 2u;
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_INVALID,
                          auto_store_save(&g_s, &KV, &p, &env, 0u, 4u, &why, NULL));
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_MAKER, why);
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_save(&g_s, &KV, &p, &env, 1u, 4u, &why, &uid));
    TEST_ASSERT_TRUE(auto_store_get(&g_s, uid, &got));
    TEST_ASSERT_EQUAL_UINT8(1u, got.made_by);
}

/* D30, AUT-009: changed_at is the store's - the time of an add or of an edit that changes the
 * sentence, kept through one that doesn't (a confirm, off, the maker), 0 when the caller can't
 * tell the time; the request's own value is never taken. */
static void test_changed_at(void)
{
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV));
    const auto_env_t env = env_of(true);
    auto_rule_t r = arm_night();
    r.changed_at = 7u; /* never the request's */
    uint16_t uid = 0;
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_save_at(&g_s, &KV, &r, &env, 2u, 0u,
                                                           1800000000u, NULL, &uid));
    auto_rule_t got;
    TEST_ASSERT_TRUE(auto_store_get(&g_s, uid, &got));
    TEST_ASSERT_EQUAL_UINT32(1800000000u, got.changed_at);
    auto_rule_t dec;
    const kv_ent_t *e = kv_find("a01");
    TEST_ASSERT_EQUAL_INT(AUTO_REC_OK, auto_decode(e->data, e->len, &dec));
    TEST_ASSERT_EQUAL_UINT32(1800000000u, dec.changed_at);

    /* the same sentence by another user, later: kept */
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_save_at(&g_s, &KV, &got, &env, 3u, 1u,
                                                           1800000600u, NULL, NULL));
    TEST_ASSERT_TRUE(auto_store_get(&g_s, uid, &got));
    TEST_ASSERT_EQUAL_UINT32(1800000000u, got.changed_at);
    /* turned off: kept */
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_set_off(&g_s, &KV, uid, true, 2u));
    TEST_ASSERT_TRUE(auto_store_get(&g_s, uid, &got));
    TEST_ASSERT_EQUAL_UINT32(1800000000u, got.changed_at);
    /* another level: a new sentence, the new time */
    got.then[0].arm_level = ALARM_LEVEL_AWAY;
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_save_at(&g_s, &KV, &got, &env, 3u, 3u,
                                                           1800001200u, NULL, NULL));
    TEST_ASSERT_TRUE(auto_store_get(&g_s, uid, &got));
    TEST_ASSERT_EQUAL_UINT32(1800001200u, got.changed_at);
    /* "Bypass open zones" is part of the sentence too; without a time: unknown (0) */
    got.then[0].bypass_open = 1u;
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_save(&g_s, &KV, &got, &env, 3u, 4u, NULL, NULL));
    TEST_ASSERT_TRUE(auto_store_get(&g_s, uid, &got));
    TEST_ASSERT_EQUAL_UINT32(0u, got.changed_at);
    TEST_ASSERT_EQUAL_UINT8(1u, got.then[0].bypass_open);
    e = kv_find("a01");
    TEST_ASSERT_EQUAL_INT(AUTO_REC_OK, auto_decode(e->data, e->len, &dec));
    TEST_ASSERT_EQUAL_UINT8(1u, dec.then[0].bypass_open);
    /* decided 2026-10-05: with the installer option off a save holding it is refused */
    auto_env_t off = env;
    off.less_secure = false;
    auto_check_t why = AUTO_OK;
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_INVALID,
                          auto_store_save(&g_s, &KV, &got, &off, 3u, 5u, &why, NULL));
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_LESS_SECURE_OFF, why);
}

/* AUT-008 fixture (f): the ignorable 0x81 survives an off toggle byte for byte, and a change of
 * the record keeps it whatever the caller sent. */
static void test_ignorable_bytes_kept(void)
{
    kv_put("a06", AUTO_FX_F, sizeof AUTO_FX_F);
    put_meta(1u, 0u, 7u);
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV));
    static const uint8_t ignorable[] = {0x81, 0x04, 0xDE, 0xAD, 0xBE, 0xEF};
    TEST_ASSERT_EQUAL_UINT8(sizeof ignorable, g_s.rules[5].keep_len);

    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_set_off(&g_s, &KV, 6u, true, 0u));
    const kv_ent_t *e = kv_find("a06");
    TEST_ASSERT_EQUAL_MEMORY(ignorable, &e->data[e->len - sizeof ignorable], sizeof ignorable);
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV)); /* a restart between the toggles */
    TEST_ASSERT_TRUE(g_s.rules[5].off);
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_set_off(&g_s, &KV, 6u, false, 1u));
    e = kv_find("a06");
    TEST_ASSERT_EQUAL_size_t(sizeof AUTO_FX_F, e->len);
    TEST_ASSERT_EQUAL_MEMORY(AUTO_FX_F, e->data, sizeof AUTO_FX_F);

    /* a change from a page that knows nothing of 0x81 keeps it too */
    auto_rule_t r;
    TEST_ASSERT_TRUE(auto_store_get(&g_s, 6u, &r));
    r.keep_len = 0u;
    memset(r.keep, 0, sizeof r.keep);
    snprintf(r.then[0].ref, sizeof r.then[0].ref, "light.landing");
    const auto_env_t env = env_of(false);
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_save(&g_s, &KV, &r, &env, 1u, 2u, NULL, NULL));
    e = kv_find("a06");
    TEST_ASSERT_EQUAL_MEMORY(ignorable, &e->data[e->len - sizeof ignorable], sizeof ignorable);
    TEST_ASSERT_EQUAL_UINT8(sizeof ignorable, g_s.rules[5].keep_len);
}

/* AUT-008: meta is written first. A refused meta write changes nothing; a record write or an
 * erase refused after it leaves the rule as it was but spends the gen (and an add's uid), as a
 * power cut between the two would: a page loaded before is stale, and no uid is handed out twice,
 * after a restart too. */
static void test_failed_writes(void)
{
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV));
    const auto_env_t env = env_of(false);
    auto_rule_t p = porch("light.a");
    uint16_t gen = 0, next = 0;

    snprintf(g_kv.fail_set, sizeof g_kv.fail_set, "meta");
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_FAILED,
                          auto_store_save(&g_s, &KV, &p, &env, 1u, 0u, NULL, NULL));
    TEST_ASSERT_EQUAL_size_t(0u, count());
    TEST_ASSERT_EQUAL_UINT16(0u, g_s.gen);
    TEST_ASSERT_EQUAL_UINT16(1u, g_s.next_uid);
    TEST_ASSERT_NULL(kv_find("meta"));
    TEST_ASSERT_NULL(kv_find("a01"));

    snprintf(g_kv.fail_set, sizeof g_kv.fail_set, "a01");
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_FAILED,
                          auto_store_save(&g_s, &KV, &p, &env, 1u, 0u, NULL, NULL));
    TEST_ASSERT_EQUAL_size_t(0u, count());
    TEST_ASSERT_NULL(kv_find("a01"));
    TEST_ASSERT_NOT_NULL(meta_of(&gen, &next));
    TEST_ASSERT_EQUAL_UINT16(1u, gen);
    TEST_ASSERT_EQUAL_UINT16(2u, next);
    TEST_ASSERT_EQUAL_UINT16(1u, g_s.gen);
    TEST_ASSERT_EQUAL_UINT16(2u, g_s.next_uid);
    g_kv.fail_set[0] = '\0';
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_CHANGED, /* a page loaded before it */
                          auto_store_save(&g_s, &KV, &p, &env, 1u, 0u, NULL, NULL));
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV)); /* restart: the same */
    TEST_ASSERT_EQUAL_UINT16(1u, g_s.gen);
    TEST_ASSERT_EQUAL_UINT16(2u, g_s.next_uid);
    TEST_ASSERT_EQUAL_UINT16(2u, add(&p)); /* uid 1 is never handed out, in slot 1 */
    TEST_ASSERT_NOT_NULL(kv_find("a01"));

    /* off and delete: the record write and the erase refused after meta */
    const uint16_t before = g_s.gen;
    snprintf(g_kv.fail_set, sizeof g_kv.fail_set, "a01");
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_FAILED, auto_store_set_off(&g_s, &KV, 2u, true, g_s.gen));
    auto_rule_t got;
    TEST_ASSERT_TRUE(auto_store_get(&g_s, 2u, &got));
    TEST_ASSERT_FALSE(got.off);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(before + 1u), g_s.gen);
    snprintf(g_kv.fail_erase, sizeof g_kv.fail_erase, "a01");
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_FAILED, auto_store_delete(&g_s, &KV, 2u, g_s.gen));
    TEST_ASSERT_EQUAL_size_t(1u, count());
    TEST_ASSERT_NOT_NULL(kv_find("a01"));
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV)); /* restart: the rule, still on */
    TEST_ASSERT_TRUE(auto_store_get(&g_s, 2u, &got));
    TEST_ASSERT_FALSE(got.off);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(before + 2u), g_s.gen);
    TEST_ASSERT_EQUAL_UINT16(3u, g_s.next_uid);
}

/* AUT-008: the readers hand out copies, never the store's own rules. */
static void test_readers_copy(void)
{
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV));
    auto_rule_t p = porch("light.porch");
    (void)add(&p);
    auto_rule_t out[AUTO_MAX];
    TEST_ASSERT_EQUAL_size_t(1u, auto_store_list(&g_s, out, AUTO_MAX));
    TEST_ASSERT_EQUAL_size_t(0u, auto_store_list(&g_s, out, 0u));
    out[0].then[0].ref[0] = 'X';
    auto_rule_t got;
    TEST_ASSERT_TRUE(auto_store_get(&g_s, 1u, &got));
    TEST_ASSERT_EQUAL_STRING("light.porch", got.then[0].ref);
}

/* AUT-008: the keys a01..a16 (never renamed). */
static void test_keys(void)
{
    char k[AUTO_STORE_KEY_LEN];
    auto_store_key(1u, k);
    TEST_ASSERT_EQUAL_STRING("a01", k);
    auto_store_key(16u, k);
    TEST_ASSERT_EQUAL_STRING("a16", k);
    TEST_ASSERT_EQUAL_STRING("automations", AUTO_STORE_NAMESPACE);
    TEST_ASSERT_EQUAL_STRING("meta", AUTO_STORE_KEY_META);
}

/* AUT-015 (D25): an unlock saved while the installer option was on is kept as it is while the
 * option is off - another save and a reload leave its bytes alone and it loads OK;
 * an edit of it with the option off is refused and writes nothing; turned back on, it saves. */
static void test_option_off_keeps_a_less_secure_record(void)
{
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV));
    auto_env_t env = env_of(false);
    auto_rule_t u = porch("lock.front");
    u.when.kind = AUTO_TRIG_DISARMED;
    u.when.zone = 0;
    u.then[0].dev_kind = DEV_LOCK;
    u.then[0].dev_action = ACT_UNLOCK;
    uint16_t uid = 0;
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_save(&g_s, &KV, &u, &env, 1u, 0u, NULL, &uid));
    const kv_ent_t *e = kv_find("a01");
    TEST_ASSERT_NOT_NULL(e);
    uint8_t before[AUTO_RECORD_MAX];
    const size_t len = e->len;
    memcpy(before, e->data, len);

    env.less_secure = false;
    auto_rule_t p = porch("light.porch");
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK, auto_store_save(&g_s, &KV, &p, &env, 1u, 1u, NULL, NULL));
    TEST_ASSERT_TRUE(auto_store_load(&g_s, &KV)); /* a reload */
    auto_rule_t got;
    TEST_ASSERT_TRUE(auto_store_get(&g_s, uid, &got));
    TEST_ASSERT_EQUAL_UINT8(AUTO_REC_OK, got.rec);
    TEST_ASSERT_EQUAL_UINT8(ACT_UNLOCK, got.then[0].dev_action);
    e = kv_find("a01");
    TEST_ASSERT_EQUAL_size_t(len, e->len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(before, e->data, len);

    auto_check_t why = AUTO_OK;
    got.when.kind = AUTO_TRIG_TIME;
    got.when.minute = 600;
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_INVALID,
                          auto_store_save(&g_s, &KV, &got, &env, 1u, g_s.gen, &why, NULL));
    TEST_ASSERT_EQUAL_INT(AUTO_ERR_LESS_SECURE_OFF, why);
    e = kv_find("a01");
    TEST_ASSERT_EQUAL_HEX8_ARRAY(before, e->data, len);

    env.less_secure = true;
    TEST_ASSERT_EQUAL_INT(AUTO_SAVE_OK,
                          auto_store_save(&g_s, &KV, &got, &env, 1u, g_s.gen, &why, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_keys);
    RUN_TEST(test_load_empty);
    RUN_TEST(test_load_fixture);
    RUN_TEST(test_bad_record_kept);
    RUN_TEST(test_not_editable);
    RUN_TEST(test_read_error_read_only);
    RUN_TEST(test_meta_outcomes);
    RUN_TEST(test_meta_rebuilt);
    RUN_TEST(test_too_big_is_newer);
    RUN_TEST(test_round_trips);
    RUN_TEST(test_uid_never_reused);
    RUN_TEST(test_changed_on_stale_gen);
    RUN_TEST(test_full);
    RUN_TEST(test_no_room_only_for_adds);
    RUN_TEST(test_invalid_and_not_found);
    RUN_TEST(test_made_by_is_the_saver);
    RUN_TEST(test_option_off_keeps_a_less_secure_record);
    RUN_TEST(test_ignorable_bytes_kept);
    RUN_TEST(test_changed_at);
    RUN_TEST(test_failed_writes);
    RUN_TEST(test_readers_copy);
    return UNITY_END();
}
