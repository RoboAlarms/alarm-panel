/*
 * test_zwz_store.c - zwz_store_run, the one copy of zwave_service's persist() (docs/features/36
 * network.md §2): zwz_panel's dirty values into a fake NVS "zwave" that refuses chosen keys; and
 * the read policy of zwave_service's load_blob (zwz_store_read_max, zwz_store_read_verdict).
 *
 * Requirements: ZW-017 (keys and the node cache stored: keys first, the counter before the cache,
 * the exact order of writes), ZW-039 (the store sequence - take, store, redo, retry every 30 s -
 * host-tested; the trouble said on every pass, not latched; a stored value read at its stored
 * length up to a cap that never refuses what the firmware before read, never stored told apart
 * from refused; the decoders of keys, cfg, chk, nodes, garage and prov accept 32 extra bytes,
 * which that length-first read relies on). load_blob itself is ESP-IDF code: it is compile-checked
 * only, and calls the policy tested here. keypad_fb and keypad_ctl still need their exact length
 * (a fixed slot count and a CRC over the whole blob): pinned below so a change there is seen.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "zone_hub/zsrc_zwave.h"
#include "zw_host_crypto.h"
#include "zwave_zones/zw_feedback.h"
#include "zwave_zones/zw_keyctl.h"
#include "zwave_zones/zw_outputs.h"
#include "zwave_zones/zwz_panel.h"
#include "zwave_zones/zwz_store.h"

#define ALL_DIRTY                                                                                  \
    (ZWZ_DIRTY_NODES | ZWZ_DIRTY_KEYS | ZWZ_DIRTY_CFG | ZWZ_DIRTY_CHECKINS | ZWZ_DIRTY_GARAGE |    \
     ZWZ_DIRTY_PROV)
#define OPS_MAX 64u
#define SLOT_MAX 16384u

/* ---- a fake NVS "zwave" ---------------------------------------------------------------------- */

typedef struct {
    char key[16];
    uint8_t data[SLOT_MAX];
    size_t len;
    bool held;
} slot_t;

typedef struct {
    char ops[OPS_MAX][24]; /* "set keys", "u16 gen", "commit" - in order */
    size_t ops_n;
    const char *refuse[8]; /* keys whose set is refused */
    size_t refuse_n;
    const char *refuse_commit_after; /* the commit after this key's set is refused */
    char last_set[16];
    slot_t slots[8];
    uint16_t gen;
    bool locked;
    unsigned locks, unlocks;
    bool trouble[8]; /* what every trouble() call said */
    size_t trouble_n;
} fake_nvs_t;

static fake_nvs_t N;
static zwz_panel_t P;
static zsrc_zw_t S_SRC;
static zw_host_crypto_t HC;
static zw_crypto_port_t CRYPTO;
static zwz_store_t ST;
static uint8_t NODES_BUF[SLOT_MAX];
static uint8_t PROV_BUF[ZW_PROV_BLOB_MAX];

static void op(const char *what, const char *key)
{
    TEST_ASSERT_TRUE(N.ops_n < OPS_MAX);
    (void)snprintf(N.ops[N.ops_n++], sizeof N.ops[0], "%s%s%s", what, key[0] != '\0' ? " " : "",
                   key);
}

static bool refused(const char *key)
{
    for (size_t i = 0; i < N.refuse_n; i++) {
        if (strcmp(N.refuse[i], key) == 0) {
            return true;
        }
    }
    return false;
}

static slot_t *slot(const char *key)
{
    for (size_t i = 0; i < sizeof N.slots / sizeof N.slots[0]; i++) {
        if (N.slots[i].held && strcmp(N.slots[i].key, key) == 0) {
            return &N.slots[i];
        }
    }
    for (size_t i = 0; i < sizeof N.slots / sizeof N.slots[0]; i++) {
        if (!N.slots[i].held) {
            N.slots[i].held = true;
            (void)snprintf(N.slots[i].key, sizeof N.slots[i].key, "%s", key);
            return &N.slots[i];
        }
    }
    TEST_FAIL_MESSAGE("fake NVS full");
    return NULL;
}

static bool f_set_blob(void *ctx, const char *key, const uint8_t *data, size_t len)
{
    TEST_ASSERT_EQUAL_PTR(&N, ctx);
    TEST_ASSERT_FALSE_MESSAGE(N.locked, "NVS written under the owner's lock");
    op("set", key);
    (void)snprintf(N.last_set, sizeof N.last_set, "%s", key);
    if (refused(key) || len > SLOT_MAX) {
        return false;
    }
    slot_t *s = slot(key);
    memcpy(s->data, data, len);
    s->len = len;
    return true;
}

static bool f_set_u16(void *ctx, const char *key, uint16_t v)
{
    TEST_ASSERT_EQUAL_PTR(&N, ctx);
    TEST_ASSERT_FALSE(N.locked);
    op("u16", key);
    (void)snprintf(N.last_set, sizeof N.last_set, "%s", key);
    if (refused(key)) {
        return false;
    }
    N.gen = v;
    return true;
}

static bool f_commit(void *ctx)
{
    TEST_ASSERT_EQUAL_PTR(&N, ctx);
    op("commit", "");
    return N.refuse_commit_after == NULL || strcmp(N.refuse_commit_after, N.last_set) != 0;
}

static void f_lock(void *ctx)
{
    TEST_ASSERT_EQUAL_PTR(&N, ctx);
    TEST_ASSERT_FALSE(N.locked);
    N.locked = true;
    N.locks++;
}

static void f_unlock(void *ctx)
{
    TEST_ASSERT_EQUAL_PTR(&N, ctx);
    TEST_ASSERT_TRUE(N.locked);
    N.locked = false;
    N.unlocks++;
}

static void f_trouble(void *ctx, bool failed)
{
    TEST_ASSERT_EQUAL_PTR(&N, ctx);
    TEST_ASSERT_TRUE(N.trouble_n < sizeof N.trouble / sizeof N.trouble[0]);
    N.trouble[N.trouble_n++] = failed;
}

static zwz_store_port_t port(void)
{
    const zwz_store_port_t p = {.ctx = &N,
                                .lock = f_lock,
                                .unlock = f_unlock,
                                .set_blob = f_set_blob,
                                .set_u16 = f_set_u16,
                                .commit = f_commit,
                                .trouble = f_trouble,
                                .nodes_buf = NODES_BUF,
                                .nodes_cap = sizeof NODES_BUF,
                                .prov_buf = PROV_BUF,
                                .prov_cap = sizeof PROV_BUF};
    return p;
}

static void refuse(const char *key)
{
    N.refuse[N.refuse_n++] = key;
}

static void forget_ops(void)
{
    N.ops_n = 0u;
}

static void assert_ops(const char *const *want, size_t n)
{
    char msg[64];
    TEST_ASSERT_EQUAL_size_t(n, N.ops_n);
    for (size_t i = 0; i < n; i++) {
        (void)snprintf(msg, sizeof msg, "write %u", (unsigned)i);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want[i], N.ops[i], msg);
    }
}

static bool has_op(const char *what)
{
    for (size_t i = 0; i < N.ops_n; i++) {
        if (strcmp(N.ops[i], what) == 0) {
            return true;
        }
    }
    return false;
}

static const slot_t *stored(const char *key)
{
    for (size_t i = 0; i < sizeof N.slots / sizeof N.slots[0]; i++) {
        if (N.slots[i].held && strcmp(N.slots[i].key, key) == 0) {
            return &N.slots[i];
        }
    }
    return NULL;
}

static bool no_write(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    (void)bytes;
    (void)len;
    return true;
}

void setUp(void)
{
    static const zsrc_sink_t sink = {0};
    static const zwz_panel_port_t pport = {.write = no_write};
    memset(&N, 0, sizeof N);
    zw_host_crypto_init(&HC, 0x53544F52u);
    CRYPTO = zw_host_crypto_port(&HC);
    zsrc_zw_init(&S_SRC, &sink);
    zwz_panel_setup_t setup;
    memset(&setup, 0, sizeof setup);
    setup.generation = 0x1234u;
    setup.keys.bound = true;
    setup.keys.home_id = 0xC0DE0001u; /* made up */
    zwz_panel_init(&P, &pport, &setup, NULL, 0u, &S_SRC, &CRYPTO);
    zwz_store_init(&ST);
}

void tearDown(void)
{
    TEST_ASSERT_FALSE_MESSAGE(N.locked, "the owner's lock was left held");
    TEST_ASSERT_EQUAL_UINT(N.locks, N.unlocks);
}

/* ---- tests ----------------------------------------------------------------------------------- */

/* ZW-017, ZW-039: everything due is written in one order - keys; gen, then nodes; cfg; chk;
 * garage; prov - each set then committed, outside the owner's lock; the pass says "no failure". */
static void test_exact_order_of_writes(void)
{
    const zwz_store_port_t pt = port();
    zwz_panel_redo(&P, ALL_DIRTY); /* zwz_panel_redo marks values due */
    TEST_ASSERT_EQUAL_UINT8(0u, zwz_store_run(&ST, &P, 1000u, &pt));
    static const char *const want[] = {"set keys", "commit",     "u16 gen",    "commit",
                                       "set nodes", "commit",    "set cfg",    "commit",
                                       "set chk",   "commit",    "set garage", "commit",
                                       "set prov",  "commit"};
    assert_ops(want, sizeof want / sizeof want[0]);
    TEST_ASSERT_EQUAL_UINT8(0u, zwz_panel_dirty(&P));
    TEST_ASSERT_EQUAL_size_t(1u, N.trouble_n);
    TEST_ASSERT_FALSE(N.trouble[0]);
    TEST_ASSERT_EQUAL_HEX16(0x1234u, N.gen);
    TEST_ASSERT_EQUAL_UINT(2u, N.locks); /* the take, then the node cache stored under it (ZW-014) */
    const slot_t *k = stored("keys");
    TEST_ASSERT_NOT_NULL(k);
    zwz_keys_t keys;
    TEST_ASSERT_TRUE(zwz_keys_decode(&keys, k->data, k->len));
    TEST_ASSERT_TRUE(keys.bound);
    TEST_ASSERT_EQUAL_HEX32(0xC0DE0001u, keys.home_id);
    /* nothing due: nothing written, nothing said */
    forget_ops();
    TEST_ASSERT_EQUAL_UINT8(0u, zwz_store_run(&ST, &P, 2000u, &pt));
    TEST_ASSERT_EQUAL_size_t(0u, N.ops_n);
    TEST_ASSERT_EQUAL_size_t(1u, N.trouble_n);
}

/* ZW-039: only what is due is written - one value, then nothing. */
static void test_only_what_is_due(void)
{
    const zwz_store_port_t pt = port();
    zwz_panel_redo(&P, ZWZ_DIRTY_CFG);
    TEST_ASSERT_EQUAL_UINT8(0u, zwz_store_run(&ST, &P, 0u, &pt));
    static const char *const want[] = {"set cfg", "commit"};
    assert_ops(want, 2u);
    TEST_ASSERT_EQUAL_UINT(1u, N.locks); /* the take under it; nothing to redo or tell (ZW-014) */
}

/* ZW-039, ZW-017: the keys refused. They are first (nothing is written before them); the rest is
 * still stored; the keys alone are asked for again (zwz_panel_redo) and the pass says failed. */
static void test_keys_refused(void)
{
    const zwz_store_port_t pt = port();
    refuse("keys");
    zwz_panel_redo(&P, ALL_DIRTY);
    TEST_ASSERT_EQUAL_HEX8(ZWZ_DIRTY_KEYS, zwz_store_run(&ST, &P, 1000u, &pt));
    TEST_ASSERT_EQUAL_STRING("set keys", N.ops[0]);
    TEST_ASSERT_EQUAL_STRING("u16 gen", N.ops[1]); /* refused: no commit for it */
    TEST_ASSERT_TRUE(has_op("set nodes") && has_op("set prov"));
    TEST_ASSERT_NULL(stored("keys"));
    TEST_ASSERT_NOT_NULL(stored("nodes"));
    TEST_ASSERT_EQUAL_HEX8(ZWZ_DIRTY_KEYS, zwz_panel_dirty(&P)); /* redone */
    TEST_ASSERT_EQUAL_size_t(1u, N.trouble_n);
    TEST_ASSERT_TRUE(N.trouble[0]);
    TEST_ASSERT_EQUAL_UINT(2u, N.locks); /* the take, then the redo under the lock */
}

/* ZW-017, ZW-039: the node cache refused while the keys store - the counter went first, the
 * cache is asked for again, the keys are not. */
static void test_nodes_refused_keys_stored(void)
{
    const zwz_store_port_t pt = port();
    refuse("nodes");
    zwz_panel_redo(&P, ALL_DIRTY);
    TEST_ASSERT_EQUAL_HEX8(ZWZ_DIRTY_NODES, zwz_store_run(&ST, &P, 1000u, &pt));
    static const char *const want[] = {"set keys", "commit",  "u16 gen",    "commit",
                                       "set nodes", "set cfg", "commit",     "set chk",
                                       "commit",    "set garage", "commit", "set prov",
                                       "commit"};
    assert_ops(want, sizeof want / sizeof want[0]);
    TEST_ASSERT_NOT_NULL(stored("keys"));
    TEST_ASSERT_NULL(stored("nodes"));
    TEST_ASSERT_EQUAL_HEX8(ZWZ_DIRTY_NODES, zwz_panel_dirty(&P));
    TEST_ASSERT_TRUE(N.trouble[0]);
}

/* ZW-017: the counter refused - the cache is never written after it (a cache must not outlive its
 * counter), and both are asked for again. */
static void test_gen_refused_skips_nodes(void)
{
    const zwz_store_port_t pt = port();
    refuse("gen");
    zwz_panel_redo(&P, ZWZ_DIRTY_NODES);
    TEST_ASSERT_EQUAL_HEX8(ZWZ_DIRTY_NODES, zwz_store_run(&ST, &P, 0u, &pt));
    static const char *const want[] = {"u16 gen"};
    assert_ops(want, 1u);
    TEST_ASSERT_EQUAL_HEX8(ZWZ_DIRTY_NODES, zwz_panel_dirty(&P));
}

/* ZW-039: a set accepted but its commit refused counts as not stored. */
static void test_commit_refused(void)
{
    const zwz_store_port_t pt = port();
    N.refuse_commit_after = "chk";
    zwz_panel_redo(&P, ZWZ_DIRTY_CFG | ZWZ_DIRTY_CHECKINS);
    TEST_ASSERT_EQUAL_HEX8(ZWZ_DIRTY_CHECKINS, zwz_store_run(&ST, &P, 0u, &pt));
    TEST_ASSERT_EQUAL_HEX8(ZWZ_DIRTY_CHECKINS, zwz_panel_dirty(&P));
}

/* ZW-039: everything refused; nothing is tried again before 30 s (no write, no trouble call); at
 * the retry everything is stored and the trouble says false - not latched. */
static void test_all_refused_then_recovers_at_retry(void)
{
    const zwz_store_port_t pt = port();
    static const char *const all[] = {"keys", "gen", "nodes", "cfg", "chk", "garage", "prov"};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        refuse(all[i]);
    }
    zwz_panel_redo(&P, ALL_DIRTY);
    TEST_ASSERT_EQUAL_HEX8(ALL_DIRTY, zwz_store_run(&ST, &P, 1000u, &pt));
    TEST_ASSERT_EQUAL_HEX8(ALL_DIRTY, zwz_panel_dirty(&P));
    TEST_ASSERT_FALSE(has_op("set nodes")); /* gen refused first */
    TEST_ASSERT_EQUAL_size_t(1u, N.trouble_n);
    TEST_ASSERT_TRUE(N.trouble[0]);
    N.refuse_n = 0u; /* NVS has room again */
    forget_ops();
    TEST_ASSERT_EQUAL_UINT8(0u, zwz_store_run(&ST, &P, 1000u + ZWZ_STORE_RETRY_MS - 1u, &pt));
    TEST_ASSERT_EQUAL_size_t(0u, N.ops_n);
    TEST_ASSERT_EQUAL_size_t(1u, N.trouble_n);
    TEST_ASSERT_EQUAL_HEX8(ALL_DIRTY, zwz_panel_dirty(&P));
    TEST_ASSERT_EQUAL_UINT8(0u, zwz_store_run(&ST, &P, 1000u + ZWZ_STORE_RETRY_MS, &pt));
    TEST_ASSERT_EQUAL_size_t(14u, N.ops_n);
    TEST_ASSERT_EQUAL_size_t(2u, N.trouble_n);
    TEST_ASSERT_FALSE(N.trouble[1]);
    TEST_ASSERT_EQUAL_UINT8(0u, zwz_panel_dirty(&P));
    TEST_ASSERT_EQUAL_HEX16(0x1234u, N.gen);
    /* after a success no wait: the next change is written at once */
    zwz_panel_redo(&P, ZWZ_DIRTY_GARAGE);
    TEST_ASSERT_EQUAL_UINT8(0u, zwz_store_run(&ST, &P, 1000u + ZWZ_STORE_RETRY_MS + 1u, &pt));
    TEST_ASSERT_EQUAL_size_t(3u, N.trouble_n);
}

/* ZW-039: a failure again at the retry waits another 30 s and says failed again (every pass). */
static void test_still_refused_at_retry(void)
{
    const zwz_store_port_t pt = port();
    refuse("cfg");
    zwz_panel_redo(&P, ZWZ_DIRTY_CFG);
    TEST_ASSERT_EQUAL_HEX8(ZWZ_DIRTY_CFG, zwz_store_run(&ST, &P, 0u, &pt));
    TEST_ASSERT_EQUAL_HEX8(ZWZ_DIRTY_CFG, zwz_store_run(&ST, &P, ZWZ_STORE_RETRY_MS, &pt));
    forget_ops();
    TEST_ASSERT_EQUAL_UINT8(0u, zwz_store_run(&ST, &P, 2u * ZWZ_STORE_RETRY_MS - 1u, &pt));
    TEST_ASSERT_EQUAL_size_t(0u, N.ops_n);
    TEST_ASSERT_EQUAL_size_t(2u, N.trouble_n);
    TEST_ASSERT_TRUE(N.trouble[0] && N.trouble[1]);
}

/* ZW-014 (ZW31): a device gone from the cache holds new identities (Add, Replace, SmartStart)
 * until the cache without it is stored. Refused at each boundary - the counter's set or commit,
 * the cache's set or commit - the hold stays, through the retry's wait; the pass that stores it
 * ends it. A device gone after that is held for the next pass that stores the cache, not for one
 * that stores something else. */
static void test_identity_held_until_the_cache_is_stored(void)
{
    static const char *const keys[] = {"gen", "nodes"};
    for (size_t b = 0; b < 4u; b++) {
        setUp();
        const zwz_store_port_t pt = port();
        P.gone_seq++; /* as zwz_panel counts a device that left the cache */
        P.dirty |= ZWZ_DIRTY_NODES;
        TEST_ASSERT_TRUE(zwz_panel_identity_held(&P));
        if (b < 2u) {
            refuse(keys[b]);
        } else {
            N.refuse_commit_after = keys[b - 2u];
        }
        TEST_ASSERT_EQUAL_HEX8(ZWZ_DIRTY_NODES, zwz_store_run(&ST, &P, 1000u, &pt));
        TEST_ASSERT_TRUE(zwz_panel_identity_held(&P));
        N.refuse_n = 0u;
        N.refuse_commit_after = NULL;
        TEST_ASSERT_EQUAL_UINT8(0u, zwz_store_run(&ST, &P, 1000u + ZWZ_STORE_RETRY_MS - 1u, &pt));
        TEST_ASSERT_TRUE(zwz_panel_identity_held(&P)); /* nothing tried yet */
        TEST_ASSERT_EQUAL_UINT8(0u, zwz_store_run(&ST, &P, 1000u + ZWZ_STORE_RETRY_MS, &pt));
        TEST_ASSERT_FALSE(zwz_panel_identity_held(&P));
        TEST_ASSERT_NOT_NULL(stored("nodes"));
    }
    const zwz_store_port_t pt = port();
    P.gone_seq++;
    zwz_panel_redo(&P, ZWZ_DIRTY_CFG);
    TEST_ASSERT_EQUAL_UINT8(0u, zwz_store_run(&ST, &P, 2u * ZWZ_STORE_RETRY_MS, &pt));
    TEST_ASSERT_TRUE(zwz_panel_identity_held(&P)); /* the configuration is not the cache */
    P.dirty |= ZWZ_DIRTY_NODES;
    TEST_ASSERT_EQUAL_UINT8(0u, zwz_store_run(&ST, &P, 2u * ZWZ_STORE_RETRY_MS, &pt));
    TEST_ASSERT_FALSE(zwz_panel_identity_held(&P));
}

/* ZW-039: a value too big for its buffer is not taken (it stays dirty in zwz_panel): counted as
 * failed, and the counter is not written without its cache. A buffer big enough later stores it. */
static void test_value_too_big_for_its_buffer(void)
{
    zwz_store_port_t pt = port();
    pt.nodes_cap = 2u;
    zwz_panel_redo(&P, ZWZ_DIRTY_NODES | ZWZ_DIRTY_CFG);
    TEST_ASSERT_EQUAL_HEX8(ZWZ_DIRTY_NODES, zwz_store_run(&ST, &P, 0u, &pt));
    static const char *const want[] = {"set cfg", "commit"};
    assert_ops(want, 2u);
    TEST_ASSERT_TRUE(N.trouble[0]);
    pt.nodes_cap = sizeof NODES_BUF;
    forget_ops();
    TEST_ASSERT_EQUAL_UINT8(0u, zwz_store_run(&ST, &P, ZWZ_STORE_RETRY_MS, &pt));
    static const char *const again[] = {"u16 gen", "commit", "set nodes", "commit"};
    assert_ops(again, 4u);
    TEST_ASSERT_FALSE(N.trouble[1]);
}

/* ZW-039: the decoders of the six values zwz_store writes as blobs (keys, cfg, chk, nodes, garage,
 * prov; gen is a u16) accept what a newer firmware stored with 32 extra bytes (an OTA rollback).
 * zwave_service's load_blob relies on this when it reads the stored length, not today's size;
 * load_blob itself is not run here (ESP-IDF). */
static void test_longer_blobs_still_read(void)
{
    const zwz_store_port_t pt = port();
    zwz_panel_redo(&P, ALL_DIRTY);
    TEST_ASSERT_EQUAL_UINT8(0u, zwz_store_run(&ST, &P, 0u, &pt));
    static uint8_t longer[SLOT_MAX + 32u];
    static const char *const names[] = {"keys", "cfg", "chk", "nodes", "garage", "prov"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        const slot_t *s = stored(names[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(s, names[i]);
        memcpy(longer, s->data, s->len);
        memset(longer + s->len, 0xA5, 32u); /* what a newer firmware appended */
        const size_t n = s->len + 32u;
        bool ok = false;
        if (i == 0u) {
            zwz_keys_t k;
            ok = zwz_keys_decode(&k, longer, n);
        } else if (i == 1u) {
            zwz_cfg_t c;
            ok = zwz_cfg_decode(&c, longer, n);
        } else if (i == 2u) {
            zwz_checkin_t ck[ZWZ_PANEL_CHECKINS];
            size_t ck_n = 0u;
            ok = zwz_checkins_decode(ck, ZWZ_PANEL_CHECKINS, &ck_n, longer, n);
        } else if (i == 3u) {
            ok = zwz_panel_load_nodes(&P, longer, n);
        } else if (i == 4u) {
            ok = zwz_panel_load_garage(&P, longer, n);
        } else {
            ok = zwz_panel_load_prov(&P, longer, n);
        }
        TEST_ASSERT_TRUE_MESSAGE(ok, names[i]);
    }
}

/* ZW-039: keypad_fb and keypad_ctl are NOT read past their length: their decoders want the exact
 * size (a fixed slot count, a CRC over the whole blob), so a longer one is refused as invalid and
 * those assignments are off. Pinned so that whoever makes them tolerant updates zwz_store.h. */
static void test_keypad_blobs_need_their_exact_length(void)
{
    static uint8_t blob[ZWF_BLOB_SIZE + ZWKC_BLOB_SIZE + 32u];
    zwf_t f;
    zwf_init(&f);
    size_t n = zwf_encode(&f, blob, sizeof blob);
    TEST_ASSERT_EQUAL_size_t(ZWF_BLOB_SIZE, n);
    TEST_ASSERT_TRUE(zwf_decode(&f, blob, n));
    memset(blob + n, 0xA5, 32u);
    TEST_ASSERT_FALSE(zwf_decode(&f, blob, n + 32u));
    zwkc_t k;
    zwkc_init(&k, NULL);
    n = zwkc_encode(&k, blob, sizeof blob);
    TEST_ASSERT_EQUAL_size_t(ZWKC_BLOB_SIZE, n);
    TEST_ASSERT_TRUE(zwkc_decode(&k, blob, n));
    memset(blob + n, 0xA5, 32u);
    TEST_ASSERT_FALSE(zwkc_decode(&k, blob, n + 32u));
}

/* ZW-039: the cap load_blob reads at - twice today's largest and 64 bytes, never under the floor,
 * so cfg and keys (read before into a ZWZ_CHECKINS_BLOB buffer) are never refused where they read
 * before; every value's cap takes today's size plus the 32 bytes above. */
static void test_read_max(void)
{
    TEST_ASSERT_EQUAL_size_t(ZWZ_STORE_READ_FLOOR, zwz_store_read_max(0u));
    TEST_ASSERT_EQUAL_size_t(ZWZ_STORE_READ_FLOOR, zwz_store_read_max(ZWZ_CFG_BLOB));
    TEST_ASSERT_EQUAL_size_t(ZWZ_STORE_READ_FLOOR, zwz_store_read_max(ZWZ_KEYS_BLOB));
    TEST_ASSERT_TRUE(zwz_store_read_max(ZWZ_CFG_BLOB) >= ZWZ_CHECKINS_BLOB);
    TEST_ASSERT_TRUE(zwz_store_read_max(ZWZ_KEYS_BLOB) >= ZWZ_CHECKINS_BLOB);
    TEST_ASSERT_EQUAL_size_t(2u * 20000u + 64u, zwz_store_read_max(20000u));
    TEST_ASSERT_EQUAL_size_t(SIZE_MAX, zwz_store_read_max(SIZE_MAX / 2u)); /* no wrap */
    static const size_t today[] = {ZWZ_KEYS_BLOB,  ZWZ_CFG_BLOB,     ZWZ_CHECKINS_BLOB,
                                   ZWO_GARAGE_BLOB, ZW_PROV_BLOB_MAX, SLOT_MAX};
    for (size_t i = 0; i < sizeof today / sizeof today[0]; i++) {
        TEST_ASSERT_TRUE(zwz_store_read_max(today[i]) >= today[i] + 32u);
    }
}

/* ZW-039: never stored is told apart from refused - absent reads as the default silently, an
 * unreadable or oversized value is refused (load_blob logs it), the rest is read at its length. */
static void test_read_verdict(void)
{
    const size_t max = zwz_store_read_max(ZWZ_CFG_BLOB);
    /* never stored, or stored empty */
    TEST_ASSERT_EQUAL_INT(ZWZ_STORE_READ_ABSENT, zwz_store_read_verdict(false, false, 0u, max));
    TEST_ASSERT_EQUAL_INT(ZWZ_STORE_READ_ABSENT, zwz_store_read_verdict(false, true, 50u, max));
    TEST_ASSERT_EQUAL_INT(ZWZ_STORE_READ_ABSENT, zwz_store_read_verdict(true, true, 0u, max));
    /* there, but its length unreadable (another NVS error) */
    TEST_ASSERT_EQUAL_INT(ZWZ_STORE_READ_REFUSED, zwz_store_read_verdict(true, false, 0u, max));
    TEST_ASSERT_EQUAL_INT(ZWZ_STORE_READ_REFUSED, zwz_store_read_verdict(true, false, 13u, max));
    /* oversized: one byte past the cap */
    TEST_ASSERT_EQUAL_INT(ZWZ_STORE_READ_REFUSED,
                          zwz_store_read_verdict(true, true, max + 1u, max));
    /* today's, shorter (older firmware), longer up to the cap: read at the stored length */
    TEST_ASSERT_EQUAL_INT(ZWZ_STORE_READ_OK, zwz_store_read_verdict(true, true, 13u, max));
    TEST_ASSERT_EQUAL_INT(ZWZ_STORE_READ_OK, zwz_store_read_verdict(true, true, 8u, max));
    TEST_ASSERT_EQUAL_INT(ZWZ_STORE_READ_OK, zwz_store_read_verdict(true, true, 13u + 32u, max));
    TEST_ASSERT_EQUAL_INT(ZWZ_STORE_READ_OK, zwz_store_read_verdict(true, true, max, max));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_exact_order_of_writes);
    RUN_TEST(test_only_what_is_due);
    RUN_TEST(test_keys_refused);
    RUN_TEST(test_nodes_refused_keys_stored);
    RUN_TEST(test_gen_refused_skips_nodes);
    RUN_TEST(test_commit_refused);
    RUN_TEST(test_all_refused_then_recovers_at_retry);
    RUN_TEST(test_still_refused_at_retry);
    RUN_TEST(test_identity_held_until_the_cache_is_stored);
    RUN_TEST(test_value_too_big_for_its_buffer);
    RUN_TEST(test_longer_blobs_still_read);
    RUN_TEST(test_keypad_blobs_need_their_exact_length);
    RUN_TEST(test_read_max);
    RUN_TEST(test_read_verdict);
    return UNITY_END();
}
