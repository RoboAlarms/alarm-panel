/*
 * test_zbz_registry.c - the panel's Zigbee devices and what of them is stored (zbz_registry.h;
 * docs/features/20 devices.md §4, testing.md §6; ZB-010, ZB-021).
 *
 * The frozen v1 fixture (registry_fixture_v1.h) is what format 1 firmware writes: a full table
 * of 64 devices - a device with every field and ZBZ_EP_MAX endpoints, a sleepy key fob whose
 * setup was waiting for it to wake when the panel restarted, and 62 plain sensors - and the meta
 * key. The tests read the frozen bytes, check every field against literals, and write them back
 * byte for byte. Never edit the fixture: a format change adds registry_fixture_v2.h (made with
 * DUMP_FIXTURE=1 build/host/test_zbz_registry) and a migration test against this one.
 * Addresses are fictional and locally administered (0x02AB_CD00_0000_00xx).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "registry_fixture_v1.h"
#include "registry_fixture_v2.h"
#include "unity.h"
#include "zigbee_zones/zbz_registry.h"

#define IEEE(n) (0x02ABCD0000000000ull + (uint64_t)(n))
#define TABLE_REST (ZBZ_DEV_MAX - 2u)

void setUp(void) {}
void tearDown(void) {}

/* ---- a store: one value per key, as NVS keeps them -------------------------------------------- */

#define STORE_KEYS (ZBZ_DEV_MAX + 1u)

typedef struct {
    char key[STORE_KEYS][ZBZ_KEY_LEN];
    uint8_t val[STORE_KEYS][ZBZ_RECORD_MAX + 8u];
    size_t len[STORE_KEYS]; /* 0: no such key */
    size_t reads;
} store_t;

static store_t g_store;
static zbz_registry_t g_reg;
static zbz_registry_t g_reg2;

static int store_find(store_t *s, const char *key)
{
    for (size_t i = 0; i < STORE_KEYS; i++) {
        if (s->len[i] > 0u && strcmp(s->key[i], key) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void store_put(store_t *s, const char *key, const uint8_t *v, size_t n)
{
    int i = store_find(s, key);
    if (i < 0) {
        for (size_t k = 0; k < STORE_KEYS; k++) {
            if (s->len[k] == 0u) {
                i = (int)k;
                break;
            }
        }
    }
    TEST_ASSERT_TRUE(i >= 0);
    TEST_ASSERT_TRUE(n > 0u && n <= sizeof(s->val[0]));
    snprintf(s->key[i], ZBZ_KEY_LEN, "%s", key);
    memcpy(s->val[i], v, n);
    s->len[i] = n;
}

static void store_erase(store_t *s, const char *key)
{
    int i = store_find(s, key);
    if (i >= 0) {
        s->len[i] = 0;
    }
}

static size_t store_read(void *ctx, const char *key, uint8_t *buf, size_t cap)
{
    store_t *s = ctx;
    s->reads++;
    int i = store_find(s, key);
    if (i < 0) {
        return 0;
    }
    if (s->len[i] <= cap) {
        memcpy(buf, s->val[i], s->len[i]);
    }
    return s->len[i]; /* more than cap: nothing read */
}

static zbz_store_port_t port(store_t *s)
{
    return (zbz_store_port_t){.ctx = s, .read = store_read};
}

/* Everything the registry has to store, into `s` (the owner's write-back). */
static size_t store_drain(zbz_registry_t *r, store_t *s)
{
    char key[ZBZ_KEY_LEN];
    uint8_t buf[ZBZ_RECORD_MAX];
    size_t len = 0;
    size_t n = 0;
    for (;;) {
        zbz_take_t t = zbz_reg_take(r, key, buf, sizeof(buf), &len);
        if (t == ZBZ_TAKE_NONE) {
            break;
        }
        if (t == ZBZ_TAKE_WRITE) {
            store_put(s, key, buf, len);
        } else {
            store_erase(s, key);
        }
        TEST_ASSERT_TRUE(++n < 200u);
    }
    return n;
}

static void store_fixture_v1(store_t *s)
{
    memset(s, 0, sizeof(*s));
    store_put(s, "meta", ZBZ_REG_FIXTURE_V1_META, sizeof(ZBZ_REG_FIXTURE_V1_META));
    store_put(s, "d00", ZBZ_REG_FIXTURE_V1_D00, sizeof(ZBZ_REG_FIXTURE_V1_D00));
    store_put(s, "d01", ZBZ_REG_FIXTURE_V1_D01, sizeof(ZBZ_REG_FIXTURE_V1_D01));
    for (size_t i = 0; i < TABLE_REST; i++) {
        char key[ZBZ_KEY_LEN];
        snprintf(key, sizeof(key), "d%02u", (unsigned)(i + 2u));
        store_put(s, key, ZBZ_REG_FIXTURE_V1_TABLE[i], sizeof(ZBZ_REG_FIXTURE_V1_TABLE[i]));
    }
}

/* ---- the fixture's devices, as the encoder was given them ------------------------------------- */

static void fixture_registry(zbz_registry_t *r)
{
    zbz_reg_init(r);
    zbz_device_t *d = zbz_reg_add(r, IEEE(1), 0x4F21u);
    snprintf(d->name, sizeof(d->name), "Front door");
    snprintf(d->manufacturer, sizeof(d->manufacturer), "ACME Sensors");
    snprintf(d->model, sizeof(d->model), "DW-100");
    snprintf(d->sw_build, sizeof(d->sw_build), "1.2.3-test");
    d->app_version = 3;
    d->hw_version = 1;
    d->manufacturer_code = 0x1234u;
    d->node_type = ZBZ_NODE_END_DEVICE;
    d->mac_caps = ZSRC_ZB_MAC_ALLOC_ADDR;
    d->power_source = 0x03u;
    d->join = ZSRC_ZB_JOIN_INSTALL_CODE;
    d->joined_at = 1790000000u;
    d->role = ZBZ_ROLE_SENSOR;
    d->setup = ZBZ_SETUP_DONE;
    d->setup_step = ZSRC_ZB_STEP_DONE;
    d->check_in_s = 3300u;
    d->ep_count = 4;
    d->ep[0] = (zbz_endpoint_t){1, 0x0104u, 0x0402u,
                                ZSRC_ZB_CL_IAS_ZONE | ZSRC_ZB_CL_POWER_CFG | ZSRC_ZB_CL_POLL_CTRL |
                                    ZSRC_ZB_CL_IDENTIFY | ZSRC_ZB_CL_BASIC,
                                ZSRC_ZB_CL_OTA, ZSRC_ZB_ZONE_TYPE_CONTACT};
    d->ep[1] = (zbz_endpoint_t){2, 0x0104u, 0x0302u, ZSRC_ZB_CL_TEMP | ZSRC_ZB_CL_IDENTIFY, 0,
                                ZSRC_ZB_ZONE_TYPE_NONE};
    d->ep[2] = (zbz_endpoint_t){3, 0xC2DFu, 0x0001u, 0, 0, ZSRC_ZB_ZONE_TYPE_NONE};
    d->ep[3] = (zbz_endpoint_t){242, 0xA1E0u, 0x0061u, 0, 0, ZSRC_ZB_ZONE_TYPE_NONE};

    d = zbz_reg_add(r, IEEE(2), 0x1A2Bu);
    snprintf(d->manufacturer, sizeof(d->manufacturer), "ACME Sensors");
    snprintf(d->model, sizeof(d->model), "KF-1");
    d->node_type = ZBZ_NODE_END_DEVICE;
    d->mac_caps = ZSRC_ZB_MAC_ALLOC_ADDR;
    d->power_source = 0x03u;
    d->join = ZSRC_ZB_JOIN_STANDARD_KEY;
    d->joined_at = 1790000100u;
    d->role = ZBZ_ROLE_KEYFOB;
    d->setup = ZBZ_SETUP_WAKE; /* the panel restarted while it waited for the fob */
    d->setup_step = ZSRC_ZB_STEP_SIMPLE_DESC;
    d->ep_count = 1;
    d->ep[0] = (zbz_endpoint_t){1, 0x0104u, 0x0401u, ZSRC_ZB_CL_IAS_ZONE | ZSRC_ZB_CL_BASIC,
                                ZSRC_ZB_CL_IAS_ACE, ZSRC_ZB_ZONE_TYPE_KEYFOB};

    for (uint16_t id = 3; id <= ZBZ_DEV_MAX; id++) {
        d = zbz_reg_add(r, IEEE(id), 0);
        d->join = ZSRC_ZB_JOIN_STANDARD_KEY;
        d->role = ZBZ_ROLE_SENSOR;
        d->setup = ZBZ_SETUP_DONE;
    }
}

/* DUMP_FIXTURE=1: the bytes for a new fixture file, a line per field. */
static void dump_bytes(const char *name, const uint8_t *b, size_t n, bool per_field)
{
    static const char *const fields[] = {
        "", "id", "IEEE", "short address", "name", "manufacturer", "model", "software build",
        "app version", "hardware version", "manufacturer code", "node type", "MAC capabilities",
        "power source", "how it joined", "joined at", "role", "setup", "setup step",
        "check-in", "endpoint"};
    printf("static const uint8_t %s[%u] = {\n    0x%02X, /* compat */\n", name, (unsigned)n, b[0]);
    for (size_t pos = 1; pos < n;) {
        size_t flen = b[pos + 1u];
        printf("    ");
        for (size_t k = 0; k < 2u + flen; k++) {
            printf("0x%02X,%s", b[pos + k], (k + 1u < 2u + flen) ? " " : "");
        }
        if (per_field) {
            printf(" /* %u %s */", b[pos],
                   b[pos] < sizeof(fields) / sizeof(fields[0]) ? fields[b[pos]] : "?");
        }
        printf("\n");
        pos += 2u + flen;
    }
    printf("};\n");
}

static void dump_fixture(void)
{
    fixture_registry(&g_reg);
    uint8_t b[ZBZ_RECORD_MAX];
    size_t n = zbz_meta_encode(&g_reg, b, sizeof(b));
    printf("static const uint8_t ZBZ_REG_FIXTURE_V1_META[%u] = {", (unsigned)n);
    for (size_t k = 0; k < n; k++) {
        printf("0x%02X%s", b[k], k + 1u < n ? ", " : "");
    }
    printf("};\n");
    n = zbz_dev_encode(&g_reg.dev[0], b, sizeof(b));
    dump_bytes("ZBZ_REG_FIXTURE_V1_D00", b, n, true);
    n = zbz_dev_encode(&g_reg.dev[1], b, sizeof(b));
    dump_bytes("ZBZ_REG_FIXTURE_V1_D01", b, n, true);
    n = zbz_dev_encode(&g_reg.dev[2], b, sizeof(b));
    printf("static const uint8_t ZBZ_REG_FIXTURE_V1_TABLE[%u][%u] = {\n", (unsigned)TABLE_REST,
           (unsigned)n);
    for (size_t i = 2; i < ZBZ_DEV_MAX; i++) {
        size_t m = zbz_dev_encode(&g_reg.dev[i], b, sizeof(b));
        TEST_ASSERT_EQUAL_size_t(n, m);
        printf("    {");
        for (size_t k = 0; k < m; k++) {
            printf("0x%02X%s", b[k], k + 1u < m ? ", " : "");
        }
        printf("},\n");
    }
    printf("};\n");
}

/* ---- the frozen fixture ------------------------------------------------------------------------ */

/* ZB-010, ZB-021: format 1's full table reads, every field as it was written. */
static void test_fixture_v1_reads(void)
{
    store_fixture_v1(&g_store);
    zbz_store_port_t p = port(&g_store);
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_reg_load(&g_reg, &p));
    TEST_ASSERT_EQUAL_size_t(ZBZ_DEV_MAX, zbz_reg_count(&g_reg));
    TEST_ASSERT_EQUAL_UINT16(65u, g_reg.next_id);
    TEST_ASSERT_FALSE(g_reg.read_only);
    TEST_ASSERT_TRUE(g_reg.dirty == 0u);
    TEST_ASSERT_FALSE(g_reg.meta_dirty);

    const zbz_device_t *d = zbz_reg_cget(&g_reg, 1);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_TRUE(d == &g_reg.dev[0]);
    TEST_ASSERT_TRUE(d->used);
    TEST_ASSERT_EQUAL_UINT16(1u, d->id);
    TEST_ASSERT_TRUE(d->ieee == IEEE(1));
    TEST_ASSERT_EQUAL_HEX16(0x4F21u, d->nwk);
    TEST_ASSERT_EQUAL_STRING("Front door", d->name);
    TEST_ASSERT_EQUAL_STRING("ACME Sensors", d->manufacturer);
    TEST_ASSERT_EQUAL_STRING("DW-100", d->model);
    TEST_ASSERT_EQUAL_STRING("1.2.3-test", d->sw_build);
    TEST_ASSERT_EQUAL_UINT8(3u, d->app_version);
    TEST_ASSERT_EQUAL_UINT8(1u, d->hw_version);
    TEST_ASSERT_EQUAL_HEX16(0x1234u, d->manufacturer_code);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_NODE_END_DEVICE, d->node_type);
    TEST_ASSERT_EQUAL_HEX8(0x80u, d->mac_caps);
    TEST_ASSERT_EQUAL_UINT8(3u, d->power_source);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_JOIN_INSTALL_CODE, d->join); /* ZB-021 */
    TEST_ASSERT_EQUAL_UINT32(1790000000u, d->joined_at);         /* ZB-021 */
    TEST_ASSERT_EQUAL_UINT8(ZBZ_ROLE_SENSOR, d->role);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, d->setup);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_STEP_DONE, d->setup_step);
    TEST_ASSERT_EQUAL_UINT32(3300u, d->check_in_s);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_EP_MAX, d->ep_count);
    TEST_ASSERT_EQUAL_UINT8(1u, d->ep[0].endpoint);
    TEST_ASSERT_EQUAL_HEX16(0x0104u, d->ep[0].profile);
    TEST_ASSERT_EQUAL_HEX16(0x0402u, d->ep[0].device_id);
    TEST_ASSERT_EQUAL_HEX16(0x0631u, d->ep[0].in_clusters);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_CL_OTA, d->ep[0].out_clusters);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZONE_TYPE_CONTACT, d->ep[0].zone_type);
    TEST_ASSERT_EQUAL_UINT8(2u, d->ep[1].endpoint);
    TEST_ASSERT_EQUAL_HEX16(0x0104u, d->ep[1].profile);
    TEST_ASSERT_EQUAL_HEX16(0x0302u, d->ep[1].device_id);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_CL_TEMP | ZSRC_ZB_CL_IDENTIFY, d->ep[1].in_clusters);
    TEST_ASSERT_EQUAL_HEX16(0u, d->ep[1].out_clusters);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZONE_TYPE_NONE, d->ep[1].zone_type);
    TEST_ASSERT_EQUAL_UINT8(3u, d->ep[2].endpoint);
    TEST_ASSERT_EQUAL_HEX16(0xC2DFu, d->ep[2].profile);
    TEST_ASSERT_EQUAL_HEX16(0x0001u, d->ep[2].device_id);
    TEST_ASSERT_EQUAL_HEX16(0u, d->ep[2].in_clusters);
    TEST_ASSERT_EQUAL_HEX16(0u, d->ep[2].out_clusters);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZONE_TYPE_NONE, d->ep[2].zone_type);
    TEST_ASSERT_EQUAL_UINT8(242u, d->ep[3].endpoint);
    TEST_ASSERT_EQUAL_HEX16(0xA1E0u, d->ep[3].profile);
    TEST_ASSERT_EQUAL_HEX16(0x0061u, d->ep[3].device_id);
    TEST_ASSERT_EQUAL_HEX16(0u, d->ep[3].in_clusters);
    TEST_ASSERT_EQUAL_HEX16(0u, d->ep[3].out_clusters);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZONE_TYPE_NONE, d->ep[3].zone_type);
    TEST_ASSERT_FALSE(zbz_dev_router(d));
    TEST_ASSERT_TRUE(zbz_dev_sleepy(d));

    /* The key fob: its setup was waiting for it (WAKE) and reads back STOPPED at its step. */
    d = zbz_reg_cget(&g_reg, 2);
    TEST_ASSERT_TRUE(d == &g_reg.dev[1]);
    TEST_ASSERT_TRUE(d->used);
    TEST_ASSERT_EQUAL_UINT16(2u, d->id);
    TEST_ASSERT_TRUE(d->ieee == IEEE(2));
    TEST_ASSERT_EQUAL_HEX16(0x1A2Bu, d->nwk);
    TEST_ASSERT_EQUAL_STRING("", d->name);
    TEST_ASSERT_EQUAL_STRING("ACME Sensors", d->manufacturer);
    TEST_ASSERT_EQUAL_STRING("KF-1", d->model);
    TEST_ASSERT_EQUAL_STRING("", d->sw_build);
    TEST_ASSERT_EQUAL_UINT8(0u, d->app_version);
    TEST_ASSERT_EQUAL_UINT8(0u, d->hw_version);
    TEST_ASSERT_EQUAL_HEX16(0u, d->manufacturer_code);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_NODE_END_DEVICE, d->node_type);
    TEST_ASSERT_EQUAL_HEX8(0x80u, d->mac_caps);
    TEST_ASSERT_EQUAL_UINT8(3u, d->power_source);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_JOIN_STANDARD_KEY, d->join); /* ZB-021 */
    TEST_ASSERT_EQUAL_UINT32(1790000100u, d->joined_at);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_ROLE_KEYFOB, d->role);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_STOPPED, d->setup);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_STEP_SIMPLE_DESC, d->setup_step);
    TEST_ASSERT_EQUAL_UINT32(0u, d->check_in_s);
    TEST_ASSERT_EQUAL_UINT8(1u, d->ep_count);
    TEST_ASSERT_EQUAL_UINT8(1u, d->ep[0].endpoint);
    TEST_ASSERT_EQUAL_HEX16(0x0104u, d->ep[0].profile);
    TEST_ASSERT_EQUAL_HEX16(0x0401u, d->ep[0].device_id);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_CL_BASIC | ZSRC_ZB_CL_IAS_ZONE, d->ep[0].in_clusters);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_CL_IAS_ACE, d->ep[0].out_clusters);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZONE_TYPE_KEYFOB, d->ep[0].zone_type);
    TEST_ASSERT_TRUE(zbz_dev_sleepy(d));

    /* The other 62, slot by slot: ids 3..64 in d02..d63. */
    for (size_t i = 2; i < ZBZ_DEV_MAX; i++) {
        d = &g_reg.dev[i];
        TEST_ASSERT_TRUE(d->used);
        TEST_ASSERT_EQUAL_UINT16(i + 1u, d->id);
        TEST_ASSERT_TRUE(d->ieee == IEEE(i + 1u));
        TEST_ASSERT_EQUAL_HEX16(0u, d->nwk);
        TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_JOIN_STANDARD_KEY, d->join);
        TEST_ASSERT_EQUAL_UINT8(ZBZ_ROLE_SENSOR, d->role);
        TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_DONE, d->setup);
        TEST_ASSERT_EQUAL_UINT8(0u, d->ep_count);
        TEST_ASSERT_EQUAL_STRING("", d->model);
    }
    TEST_ASSERT_NULL(zbz_reg_add(&g_reg, IEEE(100), 1)); /* ZB-010: full */
}

/* ZB-010: format 1's records written back are its bytes (the fob's setup byte now STOPPED). */
static void test_fixture_v1_writes_same_bytes(void)
{
    store_fixture_v1(&g_store);
    zbz_store_port_t p = port(&g_store);
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_reg_load(&g_reg, &p));
    uint8_t b[ZBZ_RECORD_MAX];

    TEST_ASSERT_EQUAL_size_t(sizeof(ZBZ_REG_FIXTURE_V1_META), zbz_meta_encode(&g_reg, b, sizeof(b)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ZBZ_REG_FIXTURE_V1_META, b, sizeof(ZBZ_REG_FIXTURE_V1_META));
    TEST_ASSERT_EQUAL_size_t(sizeof(ZBZ_REG_FIXTURE_V1_D00),
                             zbz_dev_encode(&g_reg.dev[0], b, sizeof(b)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ZBZ_REG_FIXTURE_V1_D00, b, sizeof(ZBZ_REG_FIXTURE_V1_D00));

    uint8_t fob[sizeof(ZBZ_REG_FIXTURE_V1_D01)];
    memcpy(fob, ZBZ_REG_FIXTURE_V1_D01, sizeof(fob));
    size_t setup_at = 0;
    for (size_t pos = 1; pos < sizeof(fob); pos += 2u + (size_t)fob[pos + 1u]) {
        if (fob[pos] == ZBZ_F_SETUP) {
            setup_at = pos + 2u;
        }
    }
    TEST_ASSERT_TRUE(setup_at > 0u);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_SETUP_WAKE, fob[setup_at]);
    fob[setup_at] = ZBZ_SETUP_STOPPED;
    TEST_ASSERT_EQUAL_size_t(sizeof(fob), zbz_dev_encode(&g_reg.dev[1], b, sizeof(b)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(fob, b, sizeof(fob));

    for (size_t i = 0; i < TABLE_REST; i++) {
        TEST_ASSERT_EQUAL_size_t(sizeof(ZBZ_REG_FIXTURE_V1_TABLE[i]),
                                 zbz_dev_encode(&g_reg.dev[i + 2u], b, sizeof(b)));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(ZBZ_REG_FIXTURE_V1_TABLE[i], b,
                                     sizeof(ZBZ_REG_FIXTURE_V1_TABLE[i]));
    }

    /* And the encoder still makes the fixture from the devices it was made from. */
    fixture_registry(&g_reg2);
    TEST_ASSERT_EQUAL_size_t(sizeof(ZBZ_REG_FIXTURE_V1_D01),
                             zbz_dev_encode(&g_reg2.dev[1], b, sizeof(b)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ZBZ_REG_FIXTURE_V1_D01, b, sizeof(ZBZ_REG_FIXTURE_V1_D01));
}

/* ---- the codec --------------------------------------------------------------------------------- */

/* ZB-010: a record survives encode and decode; zero fields aren't written. */
static void test_round_trip(void)
{
    fixture_registry(&g_reg);
    uint8_t b[ZBZ_RECORD_MAX];
    zbz_device_t back;
    for (size_t i = 0; i < 3u; i++) {
        size_t n = zbz_dev_encode(&g_reg.dev[i], b, sizeof(b));
        TEST_ASSERT_TRUE(n > 0u);
        TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_dev_decode(&back, b, n));
        uint8_t again[ZBZ_RECORD_MAX];
        g_reg.dev[1].setup = ZBZ_SETUP_STOPPED;
        n = zbz_dev_encode(&g_reg.dev[i], b, sizeof(b));
        TEST_ASSERT_EQUAL_size_t(n, zbz_dev_encode(&back, again, sizeof(again)));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(b, again, n);
    }
    /* id and address only: compat, then 4 + 10 bytes. */
    zbz_device_t d;
    memset(&d, 0, sizeof(d));
    d.used = true;
    d.id = 7;
    d.ieee = IEEE(7);
    TEST_ASSERT_EQUAL_size_t(15u, zbz_dev_encode(&d, b, sizeof(b)));

    /* The longest record fits ZBZ_RECORD_MAX; a smaller cap gets nothing. */
    memset(&d, 0x41, sizeof(d));
    d.used = true;
    d.name[ZBZ_NAME_LEN - 1u] = '\0';
    d.manufacturer[ZBZ_TEXT_LEN - 1u] = '\0';
    d.model[ZBZ_TEXT_LEN - 1u] = '\0';
    d.sw_build[ZBZ_SW_LEN - 1u] = '\0';
    d.ep_count = ZBZ_EP_MAX;
    size_t longest = zbz_dev_encode(&d, b, sizeof(b));
    TEST_ASSERT_TRUE(longest > 200u && longest <= ZBZ_RECORD_MAX);
    TEST_ASSERT_EQUAL_size_t(0u, zbz_dev_encode(&d, b, longest - 1u));
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_dev_decode(&back, b, longest));
    TEST_ASSERT_EQUAL_size_t(ZBZ_NAME_LEN - 1u, strlen(back.name));

    /* Not storable: unused, no id, no address. */
    d.ieee = 0;
    TEST_ASSERT_EQUAL_size_t(0u, zbz_dev_encode(&d, b, sizeof(b)));
    d.ieee = IEEE(1);
    d.id = 0;
    TEST_ASSERT_EQUAL_size_t(0u, zbz_dev_encode(&d, b, sizeof(b)));
    d.id = 1;
    d.used = false;
    TEST_ASSERT_EQUAL_size_t(0u, zbz_dev_encode(&d, b, sizeof(b)));
}

/* A record of id and address, then `extra` (one field). */
static size_t record_with(uint8_t *b, const uint8_t *extra, size_t n)
{
    static const uint8_t base[] = {0x01, 0x01, 0x02, 0x05, 0x00, 0x02, 0x08, 0x05,
                                   0x00, 0x00, 0x00, 0x00, 0xCD, 0xAB, 0x02};
    memcpy(b, base, sizeof(base));
    if (n > 0u) {
        memcpy(&b[sizeof(base)], extra, n);
    }
    return sizeof(base) + n;
}

/* ZB-010: damaged records: zero length, compat 0, cut short, a length past the end, no id or no
 * address, a number of another length, a short endpoint. Each leaves the device unused. */
static void test_decode_damaged(void)
{
    uint8_t b[64] = {0};
    zbz_device_t d;
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_DAMAGED, zbz_dev_decode(&d, b, 0));
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_DAMAGED, zbz_dev_decode(&d, NULL, 4));
    size_t n = record_with(b, NULL, 0);
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_dev_decode(&d, b, n));
    TEST_ASSERT_EQUAL_UINT16(5u, d.id);
    TEST_ASSERT_TRUE(d.ieee == IEEE(5));

    for (size_t cut = 1; cut < n; cut++) {
        TEST_ASSERT_EQUAL_INT(ZBZ_REG_DAMAGED, zbz_dev_decode(&d, b, cut));
        TEST_ASSERT_FALSE(d.used);
    }
    b[0] = 0;
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_DAMAGED, zbz_dev_decode(&d, b, n));
    b[0] = ZBZ_COMPAT;

    static const uint8_t past[] = {ZBZ_F_NAME, 0x09, 'A', 'l', 'e', 'x'};
    n = record_with(b, past, sizeof(past));
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_DAMAGED, zbz_dev_decode(&d, b, n));
    static const uint8_t no_id[] = {0x01, ZBZ_F_IEEE, 0x08, 0x05, 0, 0, 0, 0, 0xCD, 0xAB, 0x02};
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_DAMAGED, zbz_dev_decode(&d, no_id, sizeof(no_id)));
    static const uint8_t no_ieee[] = {0x01, ZBZ_F_ID, 0x02, 0x05, 0x00};
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_DAMAGED, zbz_dev_decode(&d, no_ieee, sizeof(no_ieee)));
    static const uint8_t zero_id[] = {0x01, ZBZ_F_ID, 0x02, 0x00, 0x00, ZBZ_F_IEEE, 0x08,
                                      0x05, 0, 0, 0, 0, 0xCD, 0xAB, 0x02};
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_DAMAGED, zbz_dev_decode(&d, zero_id, sizeof(zero_id)));
    static const uint8_t wide_join[] = {ZBZ_F_JOIN, 0x02, 0x01, 0x00};
    n = record_with(b, wide_join, sizeof(wide_join));
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_DAMAGED, zbz_dev_decode(&d, b, n));
    static const uint8_t short_ep[] = {ZBZ_F_ENDPOINT, 10, 1, 4, 1, 2, 4, 1, 0, 0, 0, 0};
    n = record_with(b, short_ep, sizeof(short_ep));
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_DAMAGED, zbz_dev_decode(&d, b, n));
    TEST_ASSERT_FALSE(d.used);
}

/* ZB-010: what a later firmware may add reads: an unknown field is skipped (and not written
 * back), an endpoint's extra bytes are dropped, endpoints past the most and endpoint 0 are
 * skipped, a longer text is cut at a character, a longer meta is read. */
static void test_decode_forward(void)
{
    uint8_t b[ZBZ_RECORD_MAX];
    zbz_device_t d;
    static const uint8_t unknown[] = {200, 3, 1, 2, 3, ZBZ_F_ROLE, 1, ZBZ_ROLE_SIREN};
    size_t n = record_with(b, unknown, sizeof(unknown));
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_dev_decode(&d, b, n));
    TEST_ASSERT_EQUAL_UINT8(ZBZ_ROLE_SIREN, d.role);
    uint8_t again[ZBZ_RECORD_MAX];
    TEST_ASSERT_EQUAL_size_t(n - 5u, zbz_dev_encode(&d, again, sizeof(again)));

    static const uint8_t ep13[] = {ZBZ_F_ENDPOINT, 13, 9, 0x04, 0x01, 0x02, 0x04,
                                   0x01, 0x00, 0x00, 0x00, 0x15, 0x00, 0xEE, 0xEE};
    n = record_with(b, ep13, sizeof(ep13));
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_dev_decode(&d, b, n));
    TEST_ASSERT_EQUAL_UINT8(1u, d.ep_count);
    TEST_ASSERT_EQUAL_UINT8(9u, d.ep[0].endpoint);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZONE_TYPE_CONTACT, d.ep[0].zone_type);

    /* Six endpoints and an endpoint 0: the first four real ones are kept. */
    n = record_with(b, NULL, 0);
    for (uint8_t e = 0; e < 7u; e++) {
        const uint8_t num = (uint8_t)((unsigned)e == 2u ? 0u : (unsigned)e + 1u);
        const uint8_t f[] = {ZBZ_F_ENDPOINT, 11, num, 0x04, 0x01, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        memcpy(&b[n], f, sizeof(f));
        n += sizeof(f);
    }
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_dev_decode(&d, b, n));
    TEST_ASSERT_EQUAL_UINT8(ZBZ_EP_MAX, d.ep_count);
    TEST_ASSERT_EQUAL_UINT8(1u, d.ep[0].endpoint);
    TEST_ASSERT_EQUAL_UINT8(2u, d.ep[1].endpoint);
    TEST_ASSERT_EQUAL_UINT8(4u, d.ep[2].endpoint);
    TEST_ASSERT_EQUAL_UINT8(5u, d.ep[3].endpoint);

    /* A 26-byte name of a later firmware: "Küche" ... cut before a split character. */
    uint8_t name[2u + 26u] = {ZBZ_F_NAME, 26};
    memset(&name[2], 'x', 26);
    name[2 + 22] = 0xC3; /* ü starts at byte 22 of 23 that fit */
    name[2 + 23] = 0xBC;
    n = record_with(b, name, sizeof(name));
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_dev_decode(&d, b, n));
    TEST_ASSERT_EQUAL_size_t(22u, strlen(d.name));
    name[2 + 22] = 'x';
    name[2 + 23] = 'x';
    n = record_with(b, name, sizeof(name));
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_dev_decode(&d, b, n));
    TEST_ASSERT_EQUAL_size_t(ZBZ_NAME_LEN - 1u, strlen(d.name));

    /* A record left RUNNING reads back STOPPED; NEW, DONE and NOT_USABLE stay. */
    const uint8_t setups[][2] = {{ZBZ_SETUP_RUNNING, ZBZ_SETUP_STOPPED},
                                 {ZBZ_SETUP_WAKE, ZBZ_SETUP_STOPPED},
                                 {ZBZ_SETUP_NOT_USABLE, ZBZ_SETUP_NOT_USABLE},
                                 {ZBZ_SETUP_DONE, ZBZ_SETUP_DONE}};
    for (size_t i = 0; i < 4u; i++) {
        const uint8_t f[] = {ZBZ_F_SETUP, 1, setups[i][0]};
        n = record_with(b, f, sizeof(f));
        TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_dev_decode(&d, b, n));
        TEST_ASSERT_EQUAL_UINT8(setups[i][1], d.setup);
    }

    /* A longer meta of the same compat: its first 4 bytes. */
    const uint8_t meta[] = {0x01, 0x10, 0x00, 0x03, 0x99, 0x99};
    zbz_reg_init(&g_reg);
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_meta_decode(&g_reg, meta, sizeof(meta)));
    TEST_ASSERT_EQUAL_UINT16(16u, g_reg.next_id);
}

/* ZB-010, ZB-023 (ZB03): a siren's MaxDuration is field 21, u16, stored plus one so that missing
 * (every record written before it, the v1 fixture's) reads as "not read" and a valid 0 is
 * stored as 1; written after the endpoints, so a record without it is byte for byte what it was;
 * an older reader skips it as any unknown id (test_decode_forward) and reads every other field
 * the same; another length is damage. */
static void test_wd_max_duration_field(void)
{
    uint8_t b[ZBZ_RECORD_MAX];
    uint8_t base[ZBZ_RECORD_MAX];
    uint8_t out[ZBZ_RECORD_MAX];
    zbz_device_t d;
    zbz_device_t plain;
    const size_t nb = record_with(base, NULL, 0);
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_dev_decode(&plain, base, nb));
    TEST_ASSERT_FALSE(plain.wd_max_read);
    TEST_ASSERT_EQUAL_size_t(nb, zbz_dev_encode(&plain, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(base, out, nb);
    /* The frozen v1 records: not read. */
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_dev_decode(&d, ZBZ_REG_FIXTURE_V1_D00,
                                                     sizeof(ZBZ_REG_FIXTURE_V1_D00)));
    TEST_ASSERT_FALSE(d.wd_max_read);
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_dev_decode(&d, ZBZ_REG_FIXTURE_V1_D01,
                                                     sizeof(ZBZ_REG_FIXTURE_V1_D01)));
    TEST_ASSERT_FALSE(d.wd_max_read);

    static const struct {
        uint8_t stored[2];
        bool read;
        uint16_t s;
    } rows[] = {
        {{0x3D, 0x00}, true, 60},     {{0xF1, 0x00}, true, 240}, {{0x01, 0x00}, true, 0},
        {{0xFF, 0xFF}, true, 0xFFFE}, {{0x00, 0x00}, false, 0},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        const uint8_t f[] = {ZBZ_F_WD_MAX_DURATION, 2, rows[i].stored[0], rows[i].stored[1]};
        const size_t n = record_with(b, f, sizeof(f));
        TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_dev_decode(&d, b, n));
        TEST_ASSERT_EQUAL(rows[i].read, d.wd_max_read);
        TEST_ASSERT_EQUAL_UINT16(rows[i].s, d.wd_max_s);
        /* Every other field as without it. */
        TEST_ASSERT_TRUE(d.used);
        TEST_ASSERT_EQUAL_UINT16(plain.id, d.id);
        TEST_ASSERT_TRUE(plain.ieee == d.ieee);
        TEST_ASSERT_EQUAL_UINT8(plain.role, d.role);
        TEST_ASSERT_EQUAL_UINT8(plain.ep_count, d.ep_count);
        /* Written back: the same bytes (a stored 0 is never written). */
        const size_t m = zbz_dev_encode(&d, out, sizeof(out));
        TEST_ASSERT_EQUAL_size_t(rows[i].read ? n : nb, m);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(rows[i].read ? b : base, out, m);
    }
    /* Golden: 60 s read is 15 02 3D 00, last. */
    d = plain;
    d.wd_max_read = true;
    d.wd_max_s = 60u;
    TEST_ASSERT_EQUAL_size_t(nb + 4u, zbz_dev_encode(&d, out, sizeof(out)));
    static const uint8_t tail[] = {0x15, 0x02, 0x3D, 0x00};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(base, out, nb);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tail, &out[nb], sizeof(tail));
    /* ZCL's invalid 0xFFFF is never stored (it can't be told from "not read" plus one). */
    d.wd_max_s = 0xFFFFu;
    TEST_ASSERT_EQUAL_size_t(nb, zbz_dev_encode(&d, out, sizeof(out)));
    /* Another length: damage. */
    static const uint8_t one[] = {ZBZ_F_WD_MAX_DURATION, 1, 0x3D};
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_DAMAGED, zbz_dev_decode(&d, b, record_with(b, one, sizeof(one))));
    static const uint8_t four[] = {ZBZ_F_WD_MAX_DURATION, 4, 0x3D, 0, 0, 0};
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_DAMAGED,
                          zbz_dev_decode(&d, b, record_with(b, four, sizeof(four))));
    TEST_ASSERT_FALSE(d.used);
    TEST_ASSERT_EQUAL_UINT(21u, ZBZ_F_WD_MAX_DURATION); /* stored: never renumbered */
}

/* ZB-010 (ZB03): the frozen v2 records (field 21, written by hand) decode to 60 s and a read 0;
 * the v1 records stay "not read"; the bytes written back are the frozen ones. */
static void test_fixture_v2_wd_max_duration(void)
{
    zbz_device_t d;
    uint8_t out[ZBZ_RECORD_MAX];
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_dev_decode(&d, ZBZ_REG_FIXTURE_V2_SIREN60,
                                                     sizeof(ZBZ_REG_FIXTURE_V2_SIREN60)));
    TEST_ASSERT_EQUAL_UINT16(5u, d.id);
    TEST_ASSERT_EQUAL_UINT8(ZBZ_ROLE_SIREN, d.role);
    TEST_ASSERT_TRUE(d.wd_max_read);
    TEST_ASSERT_EQUAL_UINT16(60u, d.wd_max_s);
    TEST_ASSERT_EQUAL_size_t(sizeof(ZBZ_REG_FIXTURE_V2_SIREN60), zbz_dev_encode(&d, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ZBZ_REG_FIXTURE_V2_SIREN60, out, sizeof(ZBZ_REG_FIXTURE_V2_SIREN60));

    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_dev_decode(&d, ZBZ_REG_FIXTURE_V2_SIREN0,
                                                     sizeof(ZBZ_REG_FIXTURE_V2_SIREN0)));
    TEST_ASSERT_EQUAL_UINT16(6u, d.id);
    TEST_ASSERT_TRUE(d.wd_max_read);
    TEST_ASSERT_EQUAL_UINT16(0u, d.wd_max_s);
    TEST_ASSERT_EQUAL_size_t(sizeof(ZBZ_REG_FIXTURE_V2_SIREN0), zbz_dev_encode(&d, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ZBZ_REG_FIXTURE_V2_SIREN0, out, sizeof(ZBZ_REG_FIXTURE_V2_SIREN0));

    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_dev_decode(&d, ZBZ_REG_FIXTURE_V1_D00,
                                                     sizeof(ZBZ_REG_FIXTURE_V1_D00)));
    TEST_ASSERT_FALSE(d.wd_max_read);
    TEST_ASSERT_EQUAL_UINT16(0u, d.wd_max_s);
}

/* ZB-010: meta's own checks. */
static void test_meta_codec(void)
{
    zbz_reg_init(&g_reg);
    uint8_t b[ZBZ_META_LEN];
    TEST_ASSERT_EQUAL_size_t(0u, zbz_meta_encode(&g_reg, b, ZBZ_META_LEN - 1u));
    TEST_ASSERT_EQUAL_size_t(ZBZ_META_LEN, zbz_meta_encode(&g_reg, b, sizeof(b)));
    const uint8_t empty_meta[] = {0x01, 0x01, 0x00, 0x00};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(empty_meta, b, sizeof(b));
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_EMPTY, zbz_meta_decode(&g_reg, b, 0));
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_DAMAGED, zbz_meta_decode(&g_reg, b, 3));
    const uint8_t zero_compat[] = {0x00, 0x05, 0x00, 0x01};
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_DAMAGED, zbz_meta_decode(&g_reg, zero_compat, 4));
    const uint8_t future[] = {0x02, 0x05, 0x00};
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_FUTURE, zbz_meta_decode(&g_reg, future, 3));
    const uint8_t zero_next[] = {0x01, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_meta_decode(&g_reg, zero_next, 4));
    TEST_ASSERT_EQUAL_UINT16(1u, g_reg.next_id);
}

/* ---- loading ----------------------------------------------------------------------------------- */

/* ZB-010: a new panel has nothing; nothing is to be written. */
static void test_load_empty(void)
{
    memset(&g_store, 0, sizeof(g_store));
    zbz_store_port_t p = port(&g_store);
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_EMPTY, zbz_reg_load(&g_reg, &p));
    TEST_ASSERT_EQUAL_size_t(1u + ZBZ_DEV_MAX, g_store.reads); /* meta and d00..d63 */
    TEST_ASSERT_EQUAL_size_t(0u, zbz_reg_count(&g_reg));
    TEST_ASSERT_EQUAL_UINT16(1u, g_reg.next_id);
    TEST_ASSERT_EQUAL_size_t(0u, store_drain(&g_reg, &g_store));
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_EMPTY, zbz_reg_load(&g_reg, NULL));
    const zbz_store_port_t none = {0};
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_EMPTY, zbz_reg_load(&g_reg, &none));
}

/* ZB-010: damaged records are dropped (and erased at the next write-back), the rest read: a cut
 * record, one too long to read, a second record of one address, a second of one id. Fewer
 * records than meta counts is DAMAGED; meta is rewritten. */
static void test_load_damaged(void)
{
    store_fixture_v1(&g_store);
    uint8_t b[ZBZ_RECORD_MAX + 8u];
    memcpy(b, ZBZ_REG_FIXTURE_V1_TABLE[1], 10);
    store_put(&g_store, "d03", b, 10); /* cut inside the address */
    memset(b, 0x01, sizeof(b));
    store_put(&g_store, "d04", b, ZBZ_RECORD_MAX + 1u); /* longer than a record can be */
    memcpy(b, ZBZ_REG_FIXTURE_V1_TABLE[3], sizeof(ZBZ_REG_FIXTURE_V1_TABLE[3]));
    b[5 + 2] = 0x01; /* d05 says IEEE(1), d00's address */
    store_put(&g_store, "d05", b, sizeof(ZBZ_REG_FIXTURE_V1_TABLE[3]));
    memcpy(b, ZBZ_REG_FIXTURE_V1_TABLE[4], sizeof(ZBZ_REG_FIXTURE_V1_TABLE[4]));
    b[3] = 0x02; /* d06 says id 2, the fob's */
    store_put(&g_store, "d06", b, sizeof(ZBZ_REG_FIXTURE_V1_TABLE[4]));

    zbz_store_port_t p = port(&g_store);
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_DAMAGED, zbz_reg_load(&g_reg, &p));
    TEST_ASSERT_EQUAL_size_t(ZBZ_DEV_MAX - 4u, zbz_reg_count(&g_reg));
    TEST_ASSERT_FALSE(g_reg.dev[3].used);
    TEST_ASSERT_FALSE(g_reg.dev[4].used);
    TEST_ASSERT_FALSE(g_reg.dev[5].used);
    TEST_ASSERT_FALSE(g_reg.dev[6].used);
    TEST_ASSERT_TRUE(zbz_reg_find(&g_reg, IEEE(1)) == &g_reg.dev[0]);
    TEST_ASSERT_TRUE(zbz_reg_get(&g_reg, 2) == &g_reg.dev[1]);
    TEST_ASSERT_EQUAL_UINT16(65u, g_reg.next_id);
    TEST_ASSERT_FALSE(g_reg.read_only);

    /* The write-back erases the four and rewrites meta, last. */
    char key[ZBZ_KEY_LEN];
    size_t len = 0;
    const char *const erased[] = {"d03", "d04", "d05", "d06"};
    for (size_t i = 0; i < 4u; i++) {
        TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_ERASE, zbz_reg_take(&g_reg, key, b, sizeof(b), &len));
        TEST_ASSERT_EQUAL_STRING(erased[i], key);
        store_erase(&g_store, key);
    }
    TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_WRITE, zbz_reg_take(&g_reg, key, b, sizeof(b), &len));
    TEST_ASSERT_EQUAL_STRING("meta", key);
    const uint8_t meta[] = {0x01, 65, 0x00, ZBZ_DEV_MAX - 4u};
    TEST_ASSERT_EQUAL_size_t(sizeof(meta), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(meta, b, sizeof(meta));
    store_put(&g_store, key, b, len);
    TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_NONE, zbz_reg_take(&g_reg, key, b, sizeof(b), &len));
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_reg_load(&g_reg, &p));

    /* Meta lost (or damaged): the records read, the next id is past the highest. */
    store_fixture_v1(&g_store);
    store_erase(&g_store, "meta");
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_reg_load(&g_reg, &p));
    TEST_ASSERT_EQUAL_UINT16(65u, g_reg.next_id);
    TEST_ASSERT_TRUE(g_reg.meta_dirty);
    const uint8_t bad_meta[] = {0x01, 0x41};
    store_put(&g_store, "meta", bad_meta, sizeof(bad_meta));
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_DAMAGED, zbz_reg_load(&g_reg, &p));
    TEST_ASSERT_EQUAL_size_t(ZBZ_DEV_MAX, zbz_reg_count(&g_reg));

    /* Meta counts one more than there is (a record lost): DAMAGED. More than it counts (a
     * write-back cut between a record and meta): read, meta rewritten. */
    store_fixture_v1(&g_store);
    store_erase(&g_store, "d10");
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_DAMAGED, zbz_reg_load(&g_reg, &p));
    TEST_ASSERT_TRUE(g_reg.meta_dirty);
    const uint8_t fewer[] = {0x01, 65, 0x00, 62};
    store_put(&g_store, "meta", fewer, sizeof(fewer));
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_reg_load(&g_reg, &p));
    TEST_ASSERT_TRUE(g_reg.meta_dirty);
    TEST_ASSERT_EQUAL_UINT16(65u, g_reg.next_id);
}

/* ZB-010: meta older than the records (written before them, a write-back cut between): the next
 * id goes past the highest id read, so an id dropped since is not given again after a restart;
 * meta is rewritten. A next id that already follows the highest (after a wrap past 65535) stays,
 * and meta is not rewritten at every start. */
static void test_load_stale_meta(void)
{
    zbz_store_port_t p = port(&g_store);
    store_fixture_v1(&g_store);
    const uint8_t stale[] = {0x01, 60, 0x00, 64}; /* ids 60..64 were given after it */
    store_put(&g_store, "meta", stale, sizeof(stale));
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_reg_load(&g_reg, &p));
    TEST_ASSERT_EQUAL_UINT16(65u, g_reg.next_id);
    TEST_ASSERT_TRUE(g_reg.meta_dirty);
    TEST_ASSERT_TRUE(zbz_reg_drop(&g_reg, 64));
    zbz_device_t *d = zbz_reg_add(&g_reg, IEEE(200), 0x1234u);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT16(65u, d->id); /* not 64, the dropped one */

    /* The highest id is 65535 and meta's next id is 1: as it should be, nothing to rewrite. */
    store_fixture_v1(&g_store);
    uint8_t b[sizeof(ZBZ_REG_FIXTURE_V1_TABLE[0])];
    memcpy(b, ZBZ_REG_FIXTURE_V1_TABLE[0], sizeof(b));
    b[3] = 0xFF; /* d02: id 0xFFFF */
    b[4] = 0xFF;
    store_put(&g_store, "d02", b, sizeof(b));
    const uint8_t wrapped[] = {0x01, 0x01, 0x00, 64};
    store_put(&g_store, "meta", wrapped, sizeof(wrapped));
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_reg_load(&g_reg, &p));
    TEST_ASSERT_EQUAL_UINT16(1u, g_reg.next_id);
    TEST_ASSERT_FALSE(g_reg.meta_dirty);
    TEST_ASSERT_TRUE(g_reg.dirty == 0u);
}

/* ZB-010: a later format is read as far as known and never written over - no add, no drop, no
 * touch, nothing to take, no redo. */
static void test_load_future(void)
{
    store_fixture_v1(&g_store);
    uint8_t b[ZBZ_RECORD_MAX];
    memcpy(b, ZBZ_REG_FIXTURE_V1_TABLE[0], sizeof(ZBZ_REG_FIXTURE_V1_TABLE[0]));
    b[0] = ZBZ_COMPAT + 1u;
    store_put(&g_store, "d02", b, sizeof(ZBZ_REG_FIXTURE_V1_TABLE[0]));
    zbz_device_t d;
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_FUTURE, zbz_dev_decode(&d, b, 1));
    TEST_ASSERT_FALSE(d.used);

    zbz_store_port_t p = port(&g_store);
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_FUTURE, zbz_reg_load(&g_reg, &p));
    TEST_ASSERT_TRUE(g_reg.read_only);
    TEST_ASSERT_EQUAL_size_t(ZBZ_DEV_MAX - 1u, zbz_reg_count(&g_reg));
    TEST_ASSERT_FALSE(g_reg.dev[2].used);
    TEST_ASSERT_EQUAL_STRING("DW-100", zbz_reg_cget(&g_reg, 1)->model);

    char key[ZBZ_KEY_LEN];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_NONE, zbz_reg_take(&g_reg, key, b, sizeof(b), &len));
    TEST_ASSERT_NULL(zbz_reg_add(&g_reg, IEEE(200), 1));
    TEST_ASSERT_FALSE(zbz_reg_drop(&g_reg, 1));
    zbz_reg_touch(&g_reg, &g_reg.dev[0]);
    zbz_reg_redo(&g_reg, "d00");
    zbz_reg_redo(&g_reg, "meta");
    TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_NONE, zbz_reg_take(&g_reg, key, b, sizeof(b), &len));

    /* A later meta alone does the same. */
    store_fixture_v1(&g_store);
    const uint8_t meta2[] = {ZBZ_COMPAT + 1u, 0x41, 0x00, 0x40, 0x07};
    store_put(&g_store, "meta", meta2, sizeof(meta2));
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_FUTURE, zbz_reg_load(&g_reg, &p));
    TEST_ASSERT_TRUE(g_reg.read_only);
    TEST_ASSERT_EQUAL_size_t(ZBZ_DEV_MAX, zbz_reg_count(&g_reg));
}

/* ZB-010, ZB-021 (BL-143): a record or meta whose compat byte is 0xFF - erased or overwritten
 * flash, never a format's compat (ZBZ_COMPAT_ERASED) - is damage, not a later format: that one
 * record is dropped and erased at the next write-back, and the registry stays writable (as FUTURE
 * it would be read-only for good: no device could be added or removed again). */
static void test_load_erased_compat(void)
{
    TEST_ASSERT_EQUAL_HEX8(0xFFu, ZBZ_COMPAT_ERASED);
    TEST_ASSERT_TRUE(ZBZ_COMPAT < ZBZ_COMPAT_ERASED);
    store_fixture_v1(&g_store);
    uint8_t b[ZBZ_RECORD_MAX];
    memcpy(b, ZBZ_REG_FIXTURE_V1_TABLE[0], sizeof(ZBZ_REG_FIXTURE_V1_TABLE[0]));
    b[0] = ZBZ_COMPAT_ERASED;
    store_put(&g_store, "d02", b, sizeof(ZBZ_REG_FIXTURE_V1_TABLE[0]));
    zbz_device_t d;
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_DAMAGED, zbz_dev_decode(&d, b, sizeof(ZBZ_REG_FIXTURE_V1_TABLE[0])));
    TEST_ASSERT_FALSE(d.used);
    const uint8_t all_ff[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_DAMAGED, zbz_dev_decode(&d, all_ff, sizeof(all_ff)));

    zbz_store_port_t p = port(&g_store);
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_DAMAGED, zbz_reg_load(&g_reg, &p));
    TEST_ASSERT_FALSE(g_reg.read_only);
    TEST_ASSERT_EQUAL_size_t(ZBZ_DEV_MAX - 1u, zbz_reg_count(&g_reg));
    TEST_ASSERT_FALSE(g_reg.dev[2].used);
    TEST_ASSERT_EQUAL_STRING("DW-100", zbz_reg_cget(&g_reg, 1)->model);
    char key[ZBZ_KEY_LEN];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_ERASE, zbz_reg_take(&g_reg, key, b, sizeof(b), &len));
    TEST_ASSERT_EQUAL_STRING("d02", key);
    store_erase(&g_store, key);
    TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_WRITE, zbz_reg_take(&g_reg, key, b, sizeof(b), &len));
    TEST_ASSERT_EQUAL_STRING("meta", key);
    TEST_ASSERT_NOT_NULL(zbz_reg_add(&g_reg, IEEE(200), 1)); /* still writable */

    /* Meta alone: its next id comes from the records, and it is rewritten. */
    store_fixture_v1(&g_store);
    const uint8_t meta_ff[] = {ZBZ_COMPAT_ERASED, 0x41, 0x00, 0x40};
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_DAMAGED, zbz_meta_decode(&g_reg, meta_ff, sizeof(meta_ff)));
    store_put(&g_store, "meta", meta_ff, sizeof(meta_ff));
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_DAMAGED, zbz_reg_load(&g_reg, &p));
    TEST_ASSERT_FALSE(g_reg.read_only);
    TEST_ASSERT_TRUE(g_reg.meta_dirty);
    TEST_ASSERT_EQUAL_size_t(ZBZ_DEV_MAX, zbz_reg_count(&g_reg));
}

/* ---- the devices ------------------------------------------------------------------------------- */

/* ZB-010: 64 devices fill the registry; their keys are d00..d63 and meta comes last; they read
 * back from the store as they were. */
static void test_full_registry(void)
{
    zbz_reg_init(&g_reg);
    for (uint16_t i = 1; i <= ZBZ_DEV_MAX; i++) {
        zbz_device_t *d = zbz_reg_add(&g_reg, IEEE(i), (uint16_t)(0x1000u + (unsigned)i));
        TEST_ASSERT_NOT_NULL(d);
        TEST_ASSERT_EQUAL_UINT16(i, d->id);
        d->join = (uint8_t)((unsigned)i % 3u); /* ZB-021: every way of joining is kept */
        zbz_reg_touch(&g_reg, d);
    }
    TEST_ASSERT_NULL(zbz_reg_add(&g_reg, IEEE(65), 1));
    TEST_ASSERT_EQUAL_size_t(ZBZ_DEV_MAX, zbz_reg_count(&g_reg));

    memset(&g_store, 0, sizeof(g_store));
    char key[ZBZ_KEY_LEN];
    uint8_t b[ZBZ_RECORD_MAX];
    size_t len = 0;
    for (size_t i = 0; i < ZBZ_DEV_MAX; i++) {
        TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_WRITE, zbz_reg_take(&g_reg, key, b, sizeof(b), &len));
        char want[ZBZ_KEY_LEN];
        snprintf(want, sizeof(want), "d%02u", (unsigned)i);
        TEST_ASSERT_EQUAL_STRING(want, key);
        store_put(&g_store, key, b, len);
    }
    TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_WRITE, zbz_reg_take(&g_reg, key, b, sizeof(b), &len));
    TEST_ASSERT_EQUAL_STRING(ZBZ_KEY_META, key);
    store_put(&g_store, key, b, len);
    TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_NONE, zbz_reg_take(&g_reg, key, b, sizeof(b), &len));

    zbz_store_port_t p = port(&g_store);
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_reg_load(&g_reg2, &p));
    TEST_ASSERT_EQUAL_size_t(ZBZ_DEV_MAX, zbz_reg_count(&g_reg2));
    TEST_ASSERT_EQUAL_UINT16(65u, g_reg2.next_id);
    for (size_t i = 0; i < ZBZ_DEV_MAX; i++) {
        TEST_ASSERT_EQUAL_UINT16(g_reg.dev[i].id, g_reg2.dev[i].id);
        TEST_ASSERT_TRUE(g_reg.dev[i].ieee == g_reg2.dev[i].ieee);
        TEST_ASSERT_EQUAL_UINT16(g_reg.dev[i].nwk, g_reg2.dev[i].nwk);
        TEST_ASSERT_EQUAL_UINT8(g_reg.dev[i].join, g_reg2.dev[i].join);
    }
}

/* ZB-010: ids are never 0 and never one in use; a dropped device's id isn't given to the next;
 * an address already there, or 0, gets no new record. */
static void test_ids(void)
{
    zbz_reg_init(&g_reg);
    zbz_device_t *a = zbz_reg_add(&g_reg, IEEE(1), 1);
    TEST_ASSERT_EQUAL_UINT16(1u, a->id);
    TEST_ASSERT_NULL(zbz_reg_add(&g_reg, IEEE(1), 2));
    TEST_ASSERT_NULL(zbz_reg_add(&g_reg, 0, 2));
    zbz_device_t *b = zbz_reg_add(&g_reg, IEEE(2), 2);
    TEST_ASSERT_EQUAL_UINT16(2u, b->id);
    TEST_ASSERT_TRUE(zbz_reg_drop(&g_reg, 2));
    TEST_ASSERT_FALSE(zbz_reg_drop(&g_reg, 2));
    TEST_ASSERT_FALSE(zbz_reg_drop(&g_reg, 0));
    TEST_ASSERT_NULL(zbz_reg_get(&g_reg, 2));
    TEST_ASSERT_NULL(zbz_reg_get(&g_reg, 0));
    TEST_ASSERT_NULL(zbz_reg_find(&g_reg, IEEE(2)));
    TEST_ASSERT_NULL(zbz_reg_find(&g_reg, 0));
    b = zbz_reg_add(&g_reg, IEEE(2), 2);
    TEST_ASSERT_EQUAL_UINT16(3u, b->id); /* not 2: a page left open on it can't show this one */
    TEST_ASSERT_TRUE(b == &g_reg.dev[1]); /* the freed slot */

    /* Past 65535 the ids go round to 1, skipping 0 and the ones in use. */
    g_reg.next_id = 0xFFFFu;
    zbz_device_t *c = zbz_reg_add(&g_reg, IEEE(3), 3);
    TEST_ASSERT_EQUAL_UINT16(0xFFFFu, c->id);
    TEST_ASSERT_EQUAL_UINT16(1u, g_reg.next_id);
    zbz_device_t *d = zbz_reg_add(&g_reg, IEEE(4), 4);
    TEST_ASSERT_EQUAL_UINT16(2u, d->id); /* 1 is in use */
    g_reg.next_id = 0;
    zbz_device_t *e = zbz_reg_add(&g_reg, IEEE(5), 5);
    TEST_ASSERT_EQUAL_UINT16(4u, e->id); /* 1, 2, 3 in use */
}

/* ZB-010: the take pattern: each change once, meta last; a failed write asked for again; a drop
 * is an erase; a buffer under ZBZ_RECORD_MAX takes nothing and loses nothing. */
static void test_take_redo(void)
{
    zbz_reg_init(&g_reg);
    zbz_device_t *a = zbz_reg_add(&g_reg, IEEE(1), 1);
    zbz_reg_add(&g_reg, IEEE(2), 2);
    char key[ZBZ_KEY_LEN];
    uint8_t b[ZBZ_RECORD_MAX];
    size_t len = 99;

    TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_NONE, zbz_reg_take(&g_reg, key, b, ZBZ_RECORD_MAX - 1u, &len));
    TEST_ASSERT_EQUAL_size_t(0u, len);
    TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_WRITE, zbz_reg_take(&g_reg, key, b, sizeof(b), &len));
    TEST_ASSERT_EQUAL_STRING("d00", key);
    TEST_ASSERT_EQUAL_size_t(15u + 4u, len); /* id, address, short address */
    TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_WRITE, zbz_reg_take(&g_reg, key, b, sizeof(b), &len));
    TEST_ASSERT_EQUAL_STRING("d01", key);
    zbz_reg_redo(&g_reg, "d01"); /* the write failed */
    TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_WRITE, zbz_reg_take(&g_reg, key, b, sizeof(b), &len));
    TEST_ASSERT_EQUAL_STRING("d01", key);
    TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_WRITE, zbz_reg_take(&g_reg, key, b, sizeof(b), &len));
    TEST_ASSERT_EQUAL_STRING("meta", key);
    TEST_ASSERT_EQUAL_size_t(ZBZ_META_LEN, len);
    TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_NONE, zbz_reg_take(&g_reg, key, b, sizeof(b), &len));
    zbz_reg_redo(&g_reg, "meta");
    TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_WRITE, zbz_reg_take(&g_reg, key, b, sizeof(b), &len));
    TEST_ASSERT_EQUAL_STRING("meta", key);

    /* Keys that aren't the registry's are ignored. */
    const char *const bad[] = {"d64", "d6", "d001", "x00", "", "D00", "meta2", "dA0"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        zbz_reg_redo(&g_reg, bad[i]);
    }
    zbz_reg_redo(&g_reg, NULL);
    TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_NONE, zbz_reg_take(&g_reg, key, b, sizeof(b), &len));

    /* Touch: a changed record; a pointer that isn't one of the registry's does nothing. */
    snprintf(a->name, sizeof(a->name), "Back door");
    zbz_reg_touch(&g_reg, a);
    zbz_device_t other = *a;
    zbz_reg_touch(&g_reg, &other);
    zbz_reg_touch(&g_reg, NULL);
    TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_WRITE, zbz_reg_take(&g_reg, key, b, sizeof(b), &len));
    TEST_ASSERT_EQUAL_STRING("d00", key);
    TEST_ASSERT_EQUAL_size_t(19u + 2u + 9u, len); /* and the name */
    TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_NONE, zbz_reg_take(&g_reg, key, b, sizeof(b), &len));

    /* A drop is an erase of its key, then meta. */
    TEST_ASSERT_TRUE(zbz_reg_drop(&g_reg, 1));
    TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_ERASE, zbz_reg_take(&g_reg, key, b, sizeof(b), &len));
    TEST_ASSERT_EQUAL_STRING("d00", key);
    TEST_ASSERT_EQUAL_size_t(0u, len);
    TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_WRITE, zbz_reg_take(&g_reg, key, b, sizeof(b), &len));
    TEST_ASSERT_EQUAL_STRING("meta", key);
    TEST_ASSERT_EQUAL_UINT8(1u, b[3]);
    TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_NONE, zbz_reg_take(&g_reg, key, b, sizeof(b), &len));

    /* A record a caller broke (no address) can't be stored: its old one is erased. */
    zbz_device_t *c = zbz_reg_get(&g_reg, 2);
    c->ieee = 0;
    zbz_reg_touch(&g_reg, c);
    TEST_ASSERT_EQUAL_INT(ZBZ_TAKE_ERASE, zbz_reg_take(&g_reg, key, b, sizeof(b), &len));
    TEST_ASSERT_EQUAL_STRING("d01", key);
}

/* ---- endpoints and what the zone source is told ------------------------------------------------ */

static zsrc_zb_simple_desc_t simple(uint8_t ep, uint16_t profile, uint16_t dev,
                                    const uint16_t *in, uint8_t in_n, const uint16_t *out,
                                    uint8_t out_n)
{
    zsrc_zb_simple_desc_t s;
    memset(&s, 0, sizeof(s));
    s.endpoint = ep;
    s.profile = profile;
    s.device_id = dev;
    s.in_count = in_n;
    s.out_count = out_n;
    memcpy(s.in, in, (size_t)in_n * sizeof(uint16_t));
    if (out_n > 0u) {
        memcpy(s.out, out, (size_t)out_n * sizeof(uint16_t));
    }
    return s;
}

/* ZB-010: cluster ids as bits; endpoints kept most used first; a full device gives up its least
 * used for a better one only; a re-read endpoint is replaced and keeps its zone type. */
static void test_endpoints(void)
{
    const uint16_t ids[] = {0x0000, 0x0001, 0x0003, 0x0020, 0x0500, 0xFC57, 0xFC11};
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_CL_BASIC | ZSRC_ZB_CL_POWER_CFG | ZSRC_ZB_CL_IDENTIFY |
                                ZSRC_ZB_CL_POLL_CTRL | ZSRC_ZB_CL_IAS_ZONE,
                            zbz_cluster_bits(ids, 7));
    const uint16_t more[] = {0x0006, 0x0402, 0x0406, 0xEF00, 0x0501, 0x0502, 0x0019, 0x0B05};
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_CL_ON_OFF | ZSRC_ZB_CL_TEMP | ZSRC_ZB_CL_OCCUPANCY |
                                ZSRC_ZB_CL_TUYA | ZSRC_ZB_CL_IAS_ACE | ZSRC_ZB_CL_IAS_WD |
                                ZSRC_ZB_CL_OTA | ZSRC_ZB_CL_DIAGNOSTICS,
                            zbz_cluster_bits(more, 8));
    TEST_ASSERT_EQUAL_HEX16(0u, zbz_cluster_bits(NULL, 3));

    zbz_device_t d;
    memset(&d, 0, sizeof(d));
    const uint16_t gp_in[] = {0x0021};
    const uint16_t basic[] = {0x0000};
    const uint16_t ias[] = {0x0000, 0x0001, 0x0003, 0x0500};
    const uint16_t temp[] = {0x0402};
    const uint16_t ota[] = {0x0019};
    zsrc_zb_simple_desc_t s = simple(242, 0xA1E0u, 0x0061u, gp_in, 1, NULL, 0);
    TEST_ASSERT_TRUE(zbz_dev_set_endpoint(&d, &s));
    s = simple(3, 0xC2DFu, 0x0001u, basic, 1, NULL, 0);
    TEST_ASSERT_TRUE(zbz_dev_set_endpoint(&d, &s));
    s = simple(5, 0x0104u, 0x0000u, basic, 1, NULL, 0);
    TEST_ASSERT_TRUE(zbz_dev_set_endpoint(&d, &s));
    s = simple(2, 0x0104u, 0x0402u, ias, 4, ota, 1);
    TEST_ASSERT_TRUE(zbz_dev_set_endpoint(&d, &s));
    TEST_ASSERT_EQUAL_UINT8(4u, d.ep_count);
    /* Most used first: IAS (2), Basic only (5), then the vendor profile (3) and Green Power. */
    TEST_ASSERT_EQUAL_UINT8(2u, d.ep[0].endpoint);
    TEST_ASSERT_EQUAL_UINT8(5u, d.ep[1].endpoint);
    TEST_ASSERT_EQUAL_UINT8(3u, d.ep[2].endpoint);
    TEST_ASSERT_EQUAL_UINT8(242u, d.ep[3].endpoint);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_CL_OTA, d.ep[0].out_clusters);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZONE_TYPE_NONE, d.ep[0].zone_type);

    /* Full: a temperature endpoint takes Green Power's place (the least used, last). */
    s = simple(4, 0x0104u, 0x0302u, temp, 1, NULL, 0);
    TEST_ASSERT_TRUE(zbz_dev_set_endpoint(&d, &s));
    TEST_ASSERT_EQUAL_UINT8(4u, d.ep_count);
    TEST_ASSERT_EQUAL_UINT8(2u, d.ep[0].endpoint);
    TEST_ASSERT_EQUAL_UINT8(4u, d.ep[1].endpoint);
    TEST_ASSERT_EQUAL_UINT8(5u, d.ep[2].endpoint);
    TEST_ASSERT_EQUAL_UINT8(3u, d.ep[3].endpoint);
    /* ... but another vendor-profile endpoint doesn't. */
    s = simple(6, 0xC2DFu, 0x0002u, basic, 1, NULL, 0);
    TEST_ASSERT_FALSE(zbz_dev_set_endpoint(&d, &s));

    /* Re-read: replaced in place, its IAS zone type (read from IAS Zone) kept. */
    d.ep[0].zone_type = ZSRC_ZB_ZONE_TYPE_MOTION;
    s = simple(2, 0x0104u, 0x0402u, ias, 4, NULL, 0);
    TEST_ASSERT_TRUE(zbz_dev_set_endpoint(&d, &s));
    TEST_ASSERT_EQUAL_UINT8(4u, d.ep_count);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZONE_TYPE_MOTION, d.ep[0].zone_type);
    TEST_ASSERT_EQUAL_HEX16(0u, d.ep[0].out_clusters);
    /* ... and without IAS Zone it has none. */
    s = simple(2, 0x0104u, 0x0302u, temp, 1, NULL, 0);
    TEST_ASSERT_TRUE(zbz_dev_set_endpoint(&d, &s));
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZONE_TYPE_NONE, d.ep[0].zone_type);

    /* Endpoint numbers outside 1..240 and 242 are refused; a list longer than a report holds
     * is read to its most. */
    s = simple(0, 0x0104u, 0, basic, 1, NULL, 0);
    TEST_ASSERT_FALSE(zbz_dev_set_endpoint(&d, &s));
    s.endpoint = 241;
    TEST_ASSERT_FALSE(zbz_dev_set_endpoint(&d, &s));
    s.endpoint = 255;
    TEST_ASSERT_FALSE(zbz_dev_set_endpoint(&d, &s));
    TEST_ASSERT_FALSE(zbz_dev_set_endpoint(&d, NULL));
    memset(&d, 0, sizeof(d));
    s = simple(1, 0x0104u, 0x0402u, ias, 4, NULL, 0);
    s.in_count = 200;
    s.truncated = true;
    TEST_ASSERT_TRUE(zbz_dev_set_endpoint(&d, &s));
    TEST_ASSERT_EQUAL_HEX16(zbz_cluster_bits(ias, 4), d.ep[0].in_clusters);
}

/* ZB-010, ZB-021: what the zone source is told about an endpoint - the converter testing.md §2
 * checks against the signature rows; here two of its seed rows' shapes, with fictional data. */
static void test_dev_desc(void)
{
    store_fixture_v1(&g_store);
    zbz_store_port_t p = port(&g_store);
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_reg_load(&g_reg, &p));
    zsrc_zb_desc_t z;
    const zbz_device_t *d = zbz_reg_cget(&g_reg, 1);
    TEST_ASSERT_TRUE(zbz_dev_desc(d, 1, &z));
    TEST_ASSERT_TRUE(z.ieee == IEEE(1));
    TEST_ASSERT_EQUAL_HEX16(0x4F21u, z.nwk);
    TEST_ASSERT_EQUAL_UINT8(1u, z.endpoint);
    TEST_ASSERT_EQUAL_HEX16(0x0631u, z.clusters); /* the inputs, not the OTA client */
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZONE_TYPE_CONTACT, z.zone_type);
    TEST_ASSERT_FALSE(z.router);
    TEST_ASSERT_EQUAL_STRING("ACME Sensors", z.manufacturer);
    TEST_ASSERT_EQUAL_STRING("DW-100", z.model);
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_JOIN_INSTALL_CODE, z.join);
    TEST_ASSERT_TRUE(zbz_dev_desc(d, 2, &z));
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_CL_TEMP | ZSRC_ZB_CL_IDENTIFY, z.clusters);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZONE_TYPE_NONE, z.zone_type);
    TEST_ASSERT_FALSE(zbz_dev_desc(d, 7, &z));
    TEST_ASSERT_FALSE(zbz_dev_desc(d, 0, &z));
    TEST_ASSERT_FALSE(zbz_dev_desc(NULL, 1, &z));
    d = zbz_reg_cget(&g_reg, 2);
    TEST_ASSERT_TRUE(zbz_dev_desc(d, 1, &z));
    TEST_ASSERT_EQUAL_UINT8(ZSRC_ZB_JOIN_STANDARD_KEY, z.join);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZONE_TYPE_KEYFOB, z.zone_type);
    TEST_ASSERT_EQUAL_STRING("", z.manufacturer + 12); /* NUL-ended inside 32 bytes */
}

/* ZB-010: router or sleepy from the node descriptor, else the MAC capabilities, else the power
 * source. */
static void test_router_sleepy(void)
{
    zbz_device_t d;
    memset(&d, 0, sizeof(d));
    TEST_ASSERT_FALSE(zbz_dev_router(&d)); /* nothing known */
    TEST_ASSERT_FALSE(zbz_dev_sleepy(&d));
    d.power_source = 0x01; /* mains */
    TEST_ASSERT_TRUE(zbz_dev_router(&d));
    TEST_ASSERT_FALSE(zbz_dev_sleepy(&d));
    d.power_source = 0x83; /* battery, bit 7 a backup */
    TEST_ASSERT_FALSE(zbz_dev_router(&d));
    TEST_ASSERT_TRUE(zbz_dev_sleepy(&d));
    d.power_source = 0x04; /* DC */
    TEST_ASSERT_TRUE(zbz_dev_router(&d));
    d.mac_caps = ZSRC_ZB_MAC_ALLOC_ADDR; /* an RFD, not listening: wins over the power source */
    TEST_ASSERT_FALSE(zbz_dev_router(&d));
    TEST_ASSERT_TRUE(zbz_dev_sleepy(&d));
    d.mac_caps = ZSRC_ZB_MAC_ALLOC_ADDR | ZSRC_ZB_MAC_RX_ON_IDLE;
    TEST_ASSERT_FALSE(zbz_dev_sleepy(&d));
    d.mac_caps = 142; /* FFD, mains, listening, allocate address (a siren) */
    TEST_ASSERT_TRUE(zbz_dev_router(&d));
    TEST_ASSERT_FALSE(zbz_dev_sleepy(&d));
    d.node_type = ZBZ_NODE_END_DEVICE; /* the node descriptor wins */
    TEST_ASSERT_FALSE(zbz_dev_router(&d));
    TEST_ASSERT_FALSE(zbz_dev_sleepy(&d)); /* receiver on when idle */
    d.mac_caps = ZSRC_ZB_MAC_ALLOC_ADDR;
    TEST_ASSERT_TRUE(zbz_dev_sleepy(&d));
    d.node_type = ZBZ_NODE_ROUTER;
    TEST_ASSERT_TRUE(zbz_dev_router(&d));
    TEST_ASSERT_FALSE(zbz_dev_sleepy(&d));
}

int main(void)
{
    if (getenv("DUMP_FIXTURE") != NULL) { /* prints the bytes for a new fixture file */
        dump_fixture();
        return 0;
    }
    UNITY_BEGIN();
    RUN_TEST(test_fixture_v1_reads);
    RUN_TEST(test_fixture_v1_writes_same_bytes);
    RUN_TEST(test_fixture_v2_wd_max_duration);
    RUN_TEST(test_round_trip);
    RUN_TEST(test_decode_damaged);
    RUN_TEST(test_decode_forward);
    RUN_TEST(test_wd_max_duration_field);
    RUN_TEST(test_meta_codec);
    RUN_TEST(test_load_empty);
    RUN_TEST(test_load_damaged);
    RUN_TEST(test_load_stale_meta);
    RUN_TEST(test_load_future);
    RUN_TEST(test_load_erased_compat);
    RUN_TEST(test_full_registry);
    RUN_TEST(test_ids);
    RUN_TEST(test_take_redo);
    RUN_TEST(test_endpoints);
    RUN_TEST(test_dev_desc);
    RUN_TEST(test_router_sleepy);
    return UNITY_END();
}
