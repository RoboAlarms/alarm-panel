/*
 * test_zw_smartstart.c - SmartStart (docs/features/36 network.md §4.2; ZW-024): the provisioning
 * list and its stored blob, then zw_net against the fake ZAC93 - the listen mode kept while a
 * device waits and nothing else runs, a device that asks to join with its DSK added with Security
 * 2 on its entry's terms and without the installer, a device that is not the one on its code
 * refused and removed again, five failures stopping an entry, requests that are not ours or come
 * at the wrong time ignored.
 */
#include <stdio.h>
#include <string.h>

#include "net_harness.h"
#include "unity.h"
#include "zw_prov_fixture_v1.h"
#include "zwave_proto/zw_crc16.h"
#include "zwave_proto/zw_dsk.h"
#include "zwave_proto/zw_qr.h"
#include "zwave_proto/zw_sha1.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- Helpers ------------------------------------------------------------------------------ */

/* A SmartStart code for a device (its DSK, `keys` requested), with a correct checksum. */
static void code_for(const fake_zdev_t *d, unsigned keys, char *out, size_t cap)
{
    char tail[160];
    int n = snprintf(tail, sizeof tail, "%03u", keys);
    for (size_t g = 0; g < 8u; g++) {
        n += snprintf(&tail[n], sizeof tail - (size_t)n, "%05u",
                      ((unsigned)d->pub[g * 2u] << 8) | (unsigned)d->pub[g * 2u + 1u]);
    }
    snprintf(&tail[n], sizeof tail - (size_t)n, "0010%05u%05u" "0220%05u%05u%05u%05u",
             ((unsigned)d->generic << 8) | (unsigned)d->specific, 0x0700u, 0x027Au, 0x7000u, 0xE001u, 0x0102u);
    uint8_t h[20];
    zw_sha1(tail, strlen(tail), h);
    snprintf(out, cap, "9001%05u%s", ((unsigned)h[0] << 8) | (unsigned)h[1], tail);
}

static const zw_net_event_t *until_smart(uint64_t ms)
{
    const size_t before = H.ev_n;
    const uint64_t end = H.wire.now + ms;
    while (H.wire.now < end) {
        h_run_for(100u);
        for (size_t i = before; i < H.ev_n; i++) {
            if (H.ev[i].kind == ZW_NET_EV_SMARTSTART) {
                return &H.ev[i];
            }
        }
    }
    return NULL;
}

static size_t incl_kinds(zw_incl_event_kind_t kind)
{
    size_t n = 0u;
    for (size_t i = 0; i < H.incl_n; i++) {
        n += H.incl[i].kind == kind ? 1u : 0u;
    }
    return n;
}

static fake_zdev_t *s_dev; /* the device that resets itself after a failed join */

/* A SmartStart device whose key exchange fails resets itself and asks again (network.md §4.2). */
static void resets_when_refused(const zw_incl_event_t *ev)
{
    if (ev->kind == ZW_INCL_EV_DONE && ev->smartstart && s_dev != NULL &&
        ev->security != ZW_INCL_SEC_S2_UNAUTH && ev->security != ZW_INCL_SEC_S2_AUTH &&
        ev->security != ZW_INCL_SEC_S2_ACCESS) {
        s_dev->status = FAKE_ZDEV_OUT;
    }
}

/* A world with SmartStart on and one device, not yet powered. */
static void world(fake_zdev_t *d, uint64_t seed)
{
    h_init();
    h_device(d, seed, false);
    TEST_ASSERT_TRUE(fake_zac93_add_device(&H.zac, d));
    H.smart = true;
    s_dev = d;
    H.on_incl = resets_when_refused;
}

/* ---- The list ------------------------------------------------------------------------------ */

/* ZW-024: entries from a code and from a typed DSK; the same DSK again refreshes it */
static void test_list_entries(void)
{
    static zw_prov_t p;
    zw_prov_init(&p);
    static const char LABEL[] = "900132782003515253545541424344453132333435212223242500100435301"
                                "537022065520001000000300578";
    zw_qr_t q;
    TEST_ASSERT_EQUAL(ZW_QR_OK, zw_qr_parse(LABEL, &q));
    TEST_ASSERT_EQUAL(ZW_PROV_ADDED, zw_prov_add_qr(&p, &q, "Hall light"));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)p.n);
    TEST_ASSERT_EQUAL_HEX8(0x03, p.e[0].grant);
    TEST_ASSERT_EQUAL_HEX16(0xFFF0, p.e[0].manufacturer);
    TEST_ASSERT_EQUAL_STRING("Hall light", p.e[0].name);
    TEST_ASSERT_FALSE(p.e[0].long_range);
    TEST_ASSERT_EQUAL(ZW_PROV_WAITING, zw_prov_status(&p.e[0]));
    TEST_ASSERT_TRUE(zw_prov_waiting(&p));

    /* the same code again: refreshed, not doubled; its name kept when none is given */
    (void)zw_prov_failed(&p, q.dsk);
    TEST_ASSERT_EQUAL(ZW_PROV_UPDATED, zw_prov_add_qr(&p, &q, NULL));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)p.n);
    TEST_ASSERT_EQUAL_UINT8(0u, p.e[0].failures);
    TEST_ASSERT_EQUAL_STRING("Hall light", p.e[0].name);

    /* an S2-only code can't join by itself */
    q.version = ZW_QR_VERSION_S2;
    TEST_ASSERT_EQUAL(ZW_PROV_S2_ONLY, zw_prov_add_qr(&p, &q, NULL));
    q.version = ZW_QR_VERSION_SMARTSTART;
    q.requested_keys = 0x80u; /* S0 only */
    q.dsk[0] ^= 1u;
    TEST_ASSERT_EQUAL(ZW_PROV_INVALID, zw_prov_add_qr(&p, &q, NULL));

    /* a typed DSK: every S2 key may be granted; a long name cut, never mid-character */
    uint8_t dsk[16];
    memset(dsk, 0x42, sizeof dsk);
    TEST_ASSERT_EQUAL(ZW_PROV_ADDED,
                      zw_prov_add_dsk(&p, dsk, false, "Garage door tilt senso\xC3\xA9x"));
    TEST_ASSERT_EQUAL_HEX8(0x07, p.e[1].grant);
    TEST_ASSERT_EQUAL_UINT8(0x00, p.e[1].requested);
    TEST_ASSERT_EQUAL_STRING("Garage door tilt senso", p.e[1].name);

    /* found by its NWI home ID, only while active and waiting, only for its protocol */
    uint8_t nwi[4], auth[4];
    zw_dsk_home_ids(dsk, nwi, auth);
    TEST_ASSERT_EQUAL_INT(1, zw_prov_find_nwi(&p, nwi, false));
    TEST_ASSERT_EQUAL_INT(-1, zw_prov_find_nwi(&p, nwi, true));
    TEST_ASSERT_TRUE(zw_prov_joined(&p, dsk, 9u, 3u));
    TEST_ASSERT_EQUAL(ZW_PROV_JOINED, zw_prov_status(&p.e[1]));
    TEST_ASSERT_EQUAL_INT(-1, zw_prov_find_nwi(&p, nwi, false));
    TEST_ASSERT_EQUAL_INT(1, zw_prov_find_node(&p, 9u, 3u));
    TEST_ASSERT_EQUAL_INT(-1, zw_prov_find_node(&p, 9u, 4u)); /* another device on its node ID */
    zw_prov_node_gone(&p, 9u, 3u);
    TEST_ASSERT_EQUAL(ZW_PROV_WAITING, zw_prov_status(&p.e[1])); /* removed: it would rejoin */

    /* full */
    for (unsigned i = 2; i < ZW_PROV_MAX; i++) {
        dsk[0] = (uint8_t)i;
        TEST_ASSERT_EQUAL(ZW_PROV_ADDED, zw_prov_add_dsk(&p, dsk, false, NULL));
    }
    dsk[0] = 0xEEu;
    TEST_ASSERT_EQUAL(ZW_PROV_FULL, zw_prov_add_dsk(&p, dsk, false, NULL));
    TEST_ASSERT_TRUE(zw_prov_remove(&p, 0u));
    TEST_ASSERT_EQUAL_UINT32(ZW_PROV_MAX - 1u, (uint32_t)p.n);
    TEST_ASSERT_EQUAL_STRING("Garage door tilt senso", p.e[0].name);
    TEST_ASSERT_FALSE(zw_prov_remove(&p, ZW_PROV_MAX));
}

/* ZW-024: five failed joins stop an entry; the installer turns it on again */
static void test_list_failures(void)
{
    static zw_prov_t p;
    zw_prov_init(&p);
    uint8_t dsk[16];
    memset(dsk, 0x11, sizeof dsk);
    (void)zw_prov_add_dsk(&p, dsk, false, NULL);
    for (unsigned i = 1; i < ZW_PROV_FAILURES_MAX; i++) {
        TEST_ASSERT_FALSE(zw_prov_failed(&p, dsk));
        TEST_ASSERT_EQUAL(ZW_PROV_WAITING, zw_prov_status(&p.e[0]));
    }
    TEST_ASSERT_TRUE(zw_prov_failed(&p, dsk)); /* the fifth */
    TEST_ASSERT_EQUAL(ZW_PROV_STOPPED, zw_prov_status(&p.e[0]));
    TEST_ASSERT_FALSE(zw_prov_waiting(&p));
    TEST_ASSERT_FALSE(zw_prov_failed(&p, dsk)); /* inactive: not counted further */
    TEST_ASSERT_TRUE(zw_prov_set_active(&p, 0u, true));
    TEST_ASSERT_EQUAL(ZW_PROV_WAITING, zw_prov_status(&p.e[0]));
    TEST_ASSERT_EQUAL_UINT8(0u, p.e[0].failures);
    TEST_ASSERT_TRUE(zw_prov_set_active(&p, 0u, false));
    TEST_ASSERT_EQUAL(ZW_PROV_OFF, zw_prov_status(&p.e[0]));
}

/* ZW-024: the blob - the frozen fixture, a round trip, damage, a newer writer's longer entries */
static void test_list_blob(void)
{
    static zw_prov_t p;
    TEST_ASSERT_TRUE(zw_prov_decode(&p, ZW_PROV_FIXTURE_V1, sizeof ZW_PROV_FIXTURE_V1));
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)p.n);
    TEST_ASSERT_EQUAL_HEX8(0x0F, p.e[0].dsk[15]);
    TEST_ASSERT_EQUAL_HEX8(0x03, p.e[0].grant);
    TEST_ASSERT_TRUE(p.e[0].active);
    TEST_ASSERT_EQUAL_UINT16(5u, p.e[0].node);
    TEST_ASSERT_EQUAL_UINT16(7u, p.e[0].generation);
    TEST_ASSERT_EQUAL_HEX16(0xE001, p.e[0].product_id);
    TEST_ASSERT_EQUAL_STRING("Front door", p.e[0].name);
    TEST_ASSERT_EQUAL(ZW_PROV_JOINED, zw_prov_status(&p.e[0]));
    TEST_ASSERT_TRUE(p.e[1].long_range);
    TEST_ASSERT_EQUAL(ZW_PROV_STOPPED, zw_prov_status(&p.e[1]));
    TEST_ASSERT_EQUAL_STRING("Garage", p.e[1].name);

    /* the encoder writes the same bytes */
    uint8_t out[ZW_PROV_BLOB_MAX];
    const size_t n = zw_prov_encode(&p, out, sizeof out);
    TEST_ASSERT_EQUAL_UINT32(sizeof ZW_PROV_FIXTURE_V1, (uint32_t)n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ZW_PROV_FIXTURE_V1, out, n);
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)zw_prov_encode(&p, out, n - 1u));

    /* damage: empty list */
    out[20] ^= 0x01u;
    TEST_ASSERT_FALSE(zw_prov_decode(&p, out, n));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)p.n);
    TEST_ASSERT_FALSE(zw_prov_decode(&p, ZW_PROV_FIXTURE_V1, sizeof ZW_PROV_FIXTURE_V1 - 1u));

    /* a newer firmware's entries of 60 bytes: the known part read */
    uint8_t wide[5u + 2u * 60u + 2u];
    memcpy(wide, ZW_PROV_FIXTURE_V1, 5u);
    wide[4] = 60u;
    for (size_t i = 0; i < 2u; i++) {
        memcpy(&wide[5u + i * 60u], &ZW_PROV_FIXTURE_V1[5u + i * 56u], 56u);
        memset(&wide[5u + i * 60u + 56u], 0xA5, 4u);
    }
    const uint16_t crc = zw_crc16(ZW_CRC16_INIT, wide, sizeof wide - 2u);
    wide[sizeof wide - 2u] = (uint8_t)(crc >> 8);
    wide[sizeof wide - 1u] = (uint8_t)crc;
    TEST_ASSERT_TRUE(zw_prov_decode(&p, wide, sizeof wide));
    TEST_ASSERT_EQUAL_STRING("Garage", p.e[1].name);
    /* an entry shorter than format 1's was never written: refused */
    wide[4] = 40u;
    TEST_ASSERT_FALSE(zw_prov_decode(&p, wide, sizeof wide));
}

/* ---- The network --------------------------------------------------------------------------- */

/* ZW-024: listening only while an entry waits; a device on the list joins securely by itself */
static void test_joins_by_itself(void)
{
    static fake_zdev_t d;
    world(&d, 0x5353u);
    h_smart();
    h_run_for(500u);
    TEST_ASSERT_FALSE(H.zac.smart_listen); /* nothing waits: not listening */
    TEST_ASSERT_FALSE(fake_zac93_smartstart_ask(&H.zac, H.wire.now, &d));

    TEST_ASSERT_EQUAL(ZW_PROV_ADDED, zw_prov_add_dsk(&H.prov, d.pub, false, "Front door"));
    h_smart();
    h_run_for(500u);
    TEST_ASSERT_TRUE(H.zac.smart_listen);
    TEST_ASSERT_TRUE(zw_net_smartstart_listening(&H.net));

    TEST_ASSERT_TRUE(fake_zac93_smartstart_ask(&H.zac, H.wire.now, &d));
    const zw_net_event_t *e = until_smart(120000u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(ZW_NET_SMART_JOINED, e->smart);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(d.pub, e->dsk, 16);
    const uint16_t node = e->node;
    TEST_ASSERT_NOT_EQUAL(0u, node);
    TEST_ASSERT_EQUAL_UINT32(1u, H.zac.smart_dsk_adds);
    /* nobody was asked: the entry granted, the DSK gave the PIN */
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)incl_kinds(ZW_INCL_EV_GRANT));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)incl_kinds(ZW_INCL_EV_PIN));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)incl_kinds(ZW_INCL_EV_LISTENING));
    TEST_ASSERT_EQUAL_HEX8(0x07, h_rec(node)->security_classes);
    TEST_ASSERT_EQUAL_HEX8(0x07, d.have);
    TEST_ASSERT_EQUAL(ZW_PROV_JOINED, zw_prov_status(&H.prov.e[0]));
    TEST_ASSERT_EQUAL_UINT16(h_rec(node)->generation, H.prov.e[0].generation);
    TEST_ASSERT_TRUE(h_until_ready(node, 120000u)); /* then interviewed like any other */
    /* nothing waits any more: listening stops */
    TEST_ASSERT_FALSE(H.zac.smart_listen);
    TEST_ASSERT_FALSE(zw_net_smartstart_listening(&H.net));
}

/* ZW-024: an entry from its QR code grants only what the code requested */
static void test_code_grants_its_keys(void)
{
    static fake_zdev_t d;
    world(&d, 0x5151u);
    char code[200];
    code_for(&d, 0x01u, code, sizeof code); /* S2 Unauthenticated only */
    zw_qr_t q;
    TEST_ASSERT_EQUAL(ZW_QR_OK, zw_qr_parse(code, &q));
    TEST_ASSERT_EQUAL(ZW_PROV_ADDED, zw_prov_add_qr(&H.prov, &q, NULL));
    h_smart();
    h_run_for(500u);
    TEST_ASSERT_TRUE(fake_zac93_smartstart_ask(&H.zac, H.wire.now, &d));
    const zw_net_event_t *e = until_smart(120000u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(ZW_NET_SMART_JOINED, e->smart);
    TEST_ASSERT_EQUAL_HEX8(0x01, h_rec(e->node)->security_classes); /* not the 3 it asked for */
}

/* ZW-024: a device whose key is not the DSK of its entry is refused and removed again; five such
 * joins stop the entry, and the panel stops listening */
static void test_wrong_device_removed(void)
{
    static fake_zdev_t d;
    world(&d, 0x5757u);
    uint8_t dsk[16];
    memcpy(dsk, d.pub, sizeof dsk);
    dsk[3] ^= 0x5Au; /* the same home IDs (bytes 8-15), another key */
    TEST_ASSERT_EQUAL(ZW_PROV_ADDED, zw_prov_add_dsk(&H.prov, dsk, false, NULL));
    h_smart();
    h_run_for(500u);
    for (unsigned round = 1; round <= ZW_PROV_FAILURES_MAX; round++) {
        TEST_ASSERT_TRUE_MESSAGE(fake_zac93_smartstart_ask(&H.zac, H.wire.now, &d),
                                 "the panel should still listen");
        const zw_net_event_t *e = until_smart(120000u);
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_EQUAL(ZW_NET_SMART_FAILED, e->smart);
        const uint16_t node = e->node;
        const zw_incl_event_t *done = NULL;
        for (size_t i = H.incl_n; i > 0u && done == NULL; i--) {
            if (H.incl[i - 1u].kind == ZW_INCL_EV_DONE) {
                done = &H.incl[i - 1u];
            }
        }
        TEST_ASSERT_NOT_NULL(done);
        TEST_ASSERT_EQUAL(ZW_INCL_S2_WRONG_DEVICE, done->failure);
        TEST_ASSERT_EQUAL_HEX8(0x00, d.have); /* it got no key */
        /* removed as failed: the module's list and the panel's forget it; never interviewed */
        for (unsigned i = 0; i < 600u && h_rec(node) != NULL; i++) {
            h_run_for(100u);
        }
        TEST_ASSERT_NULL(h_rec(node));
        TEST_ASSERT_NULL(h_find(ZW_NET_EV_NODE_READY, node));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)round, H.prov.e[0].failures);
        h_run_for(500u);
    }
    TEST_ASSERT_EQUAL(ZW_PROV_STOPPED, zw_prov_status(&H.prov.e[0]));
    TEST_ASSERT_FALSE(H.zac.smart_listen);
    TEST_ASSERT_FALSE(fake_zac93_smartstart_ask(&H.zac, H.wire.now, &d));
}

/* ZW-024: requests not on the list, or while the owner says no (armed), are ignored */
static void test_ignored_requests(void)
{
    static fake_zdev_t d, other;
    world(&d, 0x4949u);
    h_device(&other, 0x4A4Au, false);
    TEST_ASSERT_TRUE(fake_zac93_add_device(&H.zac, &other));
    (void)zw_prov_add_dsk(&H.prov, d.pub, false, NULL);
    h_smart();
    h_run_for(500u);
    TEST_ASSERT_TRUE(fake_zac93_smartstart_ask(&H.zac, H.wire.now, &other));
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT32(1u, zw_net_stats(&H.net)->smartstart_ignored);
    TEST_ASSERT_EQUAL_UINT32(0u, H.zac.smart_dsk_adds);
    TEST_ASSERT_TRUE(H.zac.smart_listen); /* still listening for its own */

    /* armed: the owner says no - the mode goes off; a request that still arrives is ignored */
    H.smart = false;
    zw_net_smartstart(&H.net, H.wire.now, false);
    h_run_for(500u);
    TEST_ASSERT_FALSE(H.zac.smart_listen);
    H.zac.smart_listen = true; /* one already on its way */
    TEST_ASSERT_TRUE(fake_zac93_smartstart_ask(&H.zac, H.wire.now, &d));
    H.zac.smart_listen = false;
    h_run_for(3000u);
    TEST_ASSERT_EQUAL_UINT32(2u, zw_net_stats(&H.net)->smartstart_ignored);
    TEST_ASSERT_EQUAL_UINT32(0u, H.zac.smart_dsk_adds);
    TEST_ASSERT_NULL(h_find(ZW_NET_EV_SMARTSTART, 0u));

    /* disarmed again */
    H.smart = true;
    h_smart();
    h_run_for(500u);
    TEST_ASSERT_TRUE(H.zac.smart_listen);
}

/* ZW-024: another operation turns the listen mode off first; it comes back when nothing runs */
static void test_other_operations_pause_it(void)
{
    static fake_zdev_t d, button;
    world(&d, 0x4F50u);
    (void)zw_prov_add_dsk(&H.prov, d.pub, false, NULL);
    h_smart();
    h_run_for(500u);
    TEST_ASSERT_TRUE(H.zac.smart_listen);

    TEST_ASSERT_EQUAL_INT(ZW_REMOVE_STARTED, zw_net_remove_device(&H.net, H.wire.now, true));
    h_run_for(300u);
    TEST_ASSERT_FALSE(H.zac.smart_listen);
    TEST_ASSERT_FALSE(zw_net_smartstart_listening(&H.net));
    for (unsigned i = 0; i < 700u && H.net.op != 0; i++) {
        h_run_for(100u); /* the window runs out */
    }
    h_run_for(500u);
    TEST_ASSERT_TRUE(H.zac.smart_listen);

    /* a classic "Add a device" while it listens: the device joins the usual way */
    h_device(&button, 0x4255u, false);
    H.on_incl = NULL;
    const uint16_t node = h_include(&button);
    TEST_ASSERT_NOT_EQUAL(0u, node);
    TEST_ASSERT_EQUAL_UINT32(0u, H.zac.smart_dsk_adds);
    h_run_for(1000u);
    TEST_ASSERT_TRUE(H.zac.smart_listen); /* back once nothing runs */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_list_entries);
    RUN_TEST(test_list_failures);
    RUN_TEST(test_list_blob);
    RUN_TEST(test_joins_by_itself);
    RUN_TEST(test_code_grants_its_keys);
    RUN_TEST(test_wrong_device_removed);
    RUN_TEST(test_ignored_requests);
    RUN_TEST(test_other_operations_pause_it);
    return UNITY_END();
}
