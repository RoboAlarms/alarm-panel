/*
 * test_host_zigbee_real.c - the PC simulator's real Zigbee (host_zigbee_real.h; docs/features/20
 * testing.md §10, BL-143 ZB-M6): the pages' zb_* calls, as ui_zigbee.c makes them, over the real
 * zbz_panel, its zone source on a real zone_hub and the fake radio's virtual devices, with the
 * owner's port (the engine's part) recorded here. Each test asserts what the Zigbee pages would
 * show: the add's progress, a device's page, the stored settings read back after a restart, the
 * zone a device feeds, what is logged and the refusals.
 */
#include <stdio.h>
#include <string.h>

#include "host_zigbee_real.h"
#include "unity.h"

static ui_backend_t BE;
static zone_hub_t HUB;
static uint64_t g_now;
static bool g_allowed;
static uint32_t g_taken;

/* What the owner's side got. */
#define LOG_MAX 32u
static zbz_log_t g_log[LOG_MAX];
static unsigned g_nlog;
static int g_bell = -1;
static unsigned g_posts;
static zba_post_t g_post;              /* the last keypad or fob command the engine was given */
static alarm_result_t g_ace_answer;    /* ... and the engine's answer to it */
static struct {
    uint16_t zone;
    uint8_t ev;
} g_zev[64];
static unsigned g_nzev;

static void hub_event(void *ctx, uint16_t zone, alarm_zone_event_t ev)
{
    (void)ctx;
    if (g_nzev < 64u) {
        g_zev[g_nzev].zone = zone;
        g_zev[g_nzev].ev = (uint8_t)ev;
        g_nzev++;
    }
}

static void s_input(void *ctx, const zsrc_address_t *a, const char *st, int32_t v, uint64_t now)
{
    (void)ctx;
    (void)zone_hub_input(&HUB, a, st, v, now);
}
static void s_event(void *ctx, const zsrc_address_t *a, alarm_zone_event_t ev, uint64_t now)
{
    (void)ctx;
    (void)zone_hub_event(&HUB, a, ev, now);
}
static void s_reach(void *ctx, const zsrc_address_t *a, bool ok, uint64_t now)
{
    (void)ctx;
    (void)zone_hub_reachable(&HUB, a, ok, now);
}
static void s_link(void *ctx, uint8_t kind, bool up_now, uint64_t now)
{
    (void)ctx;
    zone_hub_link(&HUB, kind, up_now, now);
}
static void s_link_now(void *ctx, uint8_t kind, uint64_t now)
{
    (void)ctx;
    zone_hub_link_now(&HUB, kind, now);
}

static bool o_allowed(void *ctx)
{
    (void)ctx;
    return g_allowed;
}
static ui_zb_req_result_t o_code(void *ctx, const char *code)
{
    (void)ctx;
    return strcmp(code, "69420") == 0 ? UI_ZB_REQ_OK : UI_ZB_REQ_WRONG_CODE;
}
static uint32_t o_wall(void *ctx)
{
    (void)ctx;
    return 1790000000u;
}
static uint32_t o_taken(void *ctx)
{
    (void)ctx;
    return g_taken;
}
static alarm_result_t o_ace(void *ctx, const zba_post_t *post, alarm_cmd_result_t *detail)
{
    (void)ctx;
    (void)detail;
    g_posts++;
    g_post = *post;
    return g_ace_answer;
}
static void o_bell(void *ctx, bool active)
{
    (void)ctx;
    g_bell = active ? 1 : 0;
}
static void o_log(void *ctx, const zbz_log_t *e)
{
    (void)ctx;
    if (g_nlog < LOG_MAX) {
        g_log[g_nlog++] = *e;
    }
}

static unsigned logged(uint8_t type)
{
    unsigned n = 0u;
    for (unsigned i = 0; i < g_nlog; i++) {
        n += g_log[i].type == type ? 1u : 0u;
    }
    return n;
}

void setUp(void)
{
    g_now = 5000u;
    g_allowed = true;
    g_taken = 1u << 0; /* the panel's own keypad 1 */
    g_nlog = 0u;
    g_bell = -1;
    g_posts = 0u;
    memset(&g_post, 0, sizeof(g_post));
    g_ace_answer = ALARM_OK;
    g_nzev = 0u;
    const zone_hub_port_t hp = {.ctx = NULL, .zone_event = hub_event, .changed = NULL,
                                .source_trouble = NULL};
    zone_hub_init(&HUB, &hp);
    const zsrc_sink_t sink = {.ctx = NULL, .input = s_input, .event = s_event,
                              .reachable = s_reach, .link = s_link, .link_now = s_link_now};
    const host_zbr_port_t port = {.ctx = NULL, .allowed = o_allowed, .installer_code = o_code,
                                  .wall_clock = o_wall, .devices_taken = o_taken,
                                  .ace_run = o_ace, .bell_trouble = o_bell, .log = o_log};
    host_zigbee_real_init(&HUB, &sink, &port, g_now);
    zone_hub_start(&HUB, g_now);
    memset(&BE, 0, sizeof(BE));
    host_zigbee_real_backend(&BE);
}

void tearDown(void) {}

/* Time passes as host_panel_advance passes it: the hub's deadlines and the Zigbee's in order. */
static void run(uint64_t ms)
{
    const uint64_t end = g_now + ms;
    while (g_now < end) {
        uint64_t next = zone_hub_next_deadline(&HUB, g_now);
        const uint64_t z = host_zigbee_real_next_due();
        next = z < next ? z : next;
        g_now = next < end && next > g_now ? next : end;
        zone_hub_tick(&HUB, g_now);
        host_zigbee_real_run(g_now);
    }
}

static ui_zb_req_result_t ask(uint8_t kind, uint16_t id)
{
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = kind;
    req.user = 1u;
    req.id = id;
    return BE.zb_request(NULL, &req);
}

static ui_zb_progress_t progress(void)
{
    ui_zb_progress_t g;
    BE.zb_progress(NULL, &g);
    return g;
}

static ui_zb_status_t status(void)
{
    ui_zb_status_t s;
    TEST_ASSERT_TRUE(BE.zb_status(NULL, &s));
    return s;
}

static ui_zb_device_page_t page(host_zbr_dev_t which)
{
    ui_zb_device_page_t pg;
    const uint16_t id = host_zigbee_real_id(which);
    TEST_ASSERT_NOT_EQUAL_UINT16(0u, id);
    TEST_ASSERT_TRUE(BE.zb_device(NULL, id, &pg));
    return pg;
}

/* One add, as the installer does it: Add a device, the window's first 20 s, Stop, the result
 * seen. The next two virtual devices join by themselves. */
static void add_round(void)
{
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u));
    run(20000u);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_STOP, 0u));
    run(1000u);
    (void)ask(UI_ZB_REQ_DISMISS, 0u);
    run(1000u);
}

/* ZB-002, ZB-027: Add a device listens; the first two virtual devices press their buttons 3 s and
 * 6 s in, join and are set up to Ready through the fake radio; the page counts them. */
static void test_add_two_join_and_reach_ready(void)
{
    ui_zb_status_t s = status();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RADIO_WORKING, s.radio);
    TEST_ASSERT_EQUAL_UINT16(0u, s.devices);
    TEST_ASSERT_EQUAL_UINT8(15u, s.channel);

    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u));
    ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_OP_ADD, g.op);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_LISTENING, g.step);
    run(2900u);
    TEST_ASSERT_EQUAL_UINT8(0u, progress().joined_count);
    TEST_ASSERT_FALSE(host_zigbee_real_joined(HOST_ZBR_DOOR));
    run(400u);
    TEST_ASSERT_TRUE(host_zigbee_real_joined(HOST_ZBR_DOOR));
    TEST_ASSERT_FALSE(host_zigbee_real_joined(HOST_ZBR_MOTION));
    TEST_ASSERT_EQUAL_UINT8(1u, progress().joined_count);
    /* its setup takes a few seconds, step by step, as a real device's does */
    TEST_ASSERT_NOT_EQUAL_UINT8(UI_ZB_STEP_DONE, progress().joined[0].step);
    run(15000u);
    g = progress();
    TEST_ASSERT_EQUAL_UINT8(2u, g.joined_count);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.joined[0].step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_ADDED, g.joined[0].result);
    TEST_ASSERT_EQUAL_STRING("Door sensor", g.joined[0].model);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.joined[1].step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_ADDED, g.joined[1].result);
    TEST_ASSERT_EQUAL_STRING("Motion sensor", g.joined[1].model);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_LISTENING, g.step); /* the window stays open */
    TEST_ASSERT_TRUE(g.seconds_left > 150u && g.seconds_left < ZSRC_ZB_LEARN_S);
    TEST_ASSERT_FALSE(host_zigbee_real_joined(HOST_ZBR_SMOKE)); /* two, no more */
    TEST_ASSERT_EQUAL_UINT16(2u, status().devices);
    TEST_ASSERT_EQUAL_UINT(2u, logged(ZBZ_EV_ADDED));

    /* the window runs out: the add ends ADDED */
    run((uint64_t)ZSRC_ZB_LEARN_S * 1000u);
    g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_ADDED, g.result);
    /* the next add brings the next two */
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_DISMISS, 0u));
    add_round();
    TEST_ASSERT_TRUE(host_zigbee_real_joined(HOST_ZBR_SMOKE));
    TEST_ASSERT_TRUE(host_zigbee_real_joined(HOST_ZBR_WATER));
    TEST_ASSERT_EQUAL_UINT16(4u, status().devices);
}

/* ZB-012, ZB-027: a setup that stops (a step never answered, its tries spent) ends the add with
 * SETUP_NOT_FINISHED and its row DONE - not mid-setup, not asleep: the add page's ending then gives
 * no step count and no wake to wait for (ui_zigbee.c AS_NOT_FINISHED), and the device's page says
 * Set it up again, the one way it goes on. */
static void test_stopped_setup_ends_not_finished(void)
{
    /* the two that join in this window never answer IAS Zone (Stop would end the add STOPPED: the
     * window runs out instead, as it does when nobody is at the panel) */
    TEST_ASSERT_TRUE(host_zigbee_real_stall(HOST_ZBR_DOOR, (uint8_t)ZSRC_ZB_STEP_IAS_ZONE));
    TEST_ASSERT_TRUE(host_zigbee_real_stall(HOST_ZBR_MOTION, (uint8_t)ZSRC_ZB_STEP_IAS_ZONE));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_ADD, 0u));
    run((uint64_t)ZSRC_ZB_LEARN_S * 1000u + (uint64_t)ZBZ_SETUP_MAX_MS + 5u * 60000u);
    TEST_ASSERT_TRUE(host_zigbee_real_joined(HOST_ZBR_DOOR));
    TEST_ASSERT_TRUE(host_zigbee_real_joined(HOST_ZBR_MOTION));
    const ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_SETUP_NOT_FINISHED, g.result);
    TEST_ASSERT_EQUAL_UINT8(2u, g.joined_count);
    for (size_t k = 0; k < 2u; k++) {
        TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.joined[k].step);
        TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_SETUP_NOT_FINISHED, g.joined[k].result);
        TEST_ASSERT_FALSE(g.joined[k].asleep);
    }
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_DEV_SETUP_STOPPED, page(HOST_ZBR_DOOR).dev.state);
    /* answered again, Set it up again goes on to Ready */
    TEST_ASSERT_TRUE(host_zigbee_real_stall(HOST_ZBR_DOOR, (uint8_t)ZSRC_ZB_STEP_NONE));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_DISMISS, 0u));
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_SETUP, page(HOST_ZBR_DOOR).dev.id));
    TEST_ASSERT_TRUE(host_zigbee_real_press(HOST_ZBR_DOOR)); /* a sleepy one is woken for it */
    run(60000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_DEV_READY, page(HOST_ZBR_DOOR).dev.state);
}

/* ZB-027: a device's page - what it is, how it joined, its input - and its list row. */
static void test_device_page_words_data(void)
{
    add_round();
    const ui_zb_device_page_t pg = page(HOST_ZBR_DOOR);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_DEV_READY, pg.dev.state);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_KIND_SENSOR, pg.dev.kind);
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEVCLASS_DOOR, pg.dev.device_class);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_JOIN_STANDARD_KEY, pg.dev.join);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_POWER_BATTERY, pg.dev.power);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_SIGNAL_GOOD, pg.dev.signal);
    TEST_ASSERT_EQUAL_STRING("Door sensor", pg.dev.model);
    TEST_ASSERT_EQUAL_STRING("SONOFF", pg.manufacturer);
    TEST_ASSERT_EQUAL_UINT64(host_zigbee_real_ieee(HOST_ZBR_DOOR), pg.ieee);
    TEST_ASSERT_TRUE(pg.sleepy);
    TEST_ASSERT_EQUAL_UINT8(92u, pg.dev.battery_pct); /* its first battery report */
    TEST_ASSERT_FALSE(pg.dev.battery_low);
    TEST_ASSERT_EQUAL_UINT32(1790000000u, pg.joined_at);
    TEST_ASSERT_EQUAL_UINT8(1u, pg.input_count);
    TEST_ASSERT_EQUAL_UINT16(0u, pg.inputs[0].zone);
    const ui_zb_device_page_t m = page(HOST_ZBR_MOTION);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_DEV_READY, m.dev.state);
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEVCLASS_MOTION, m.dev.device_class);

    ui_zb_device_t rows[8];
    TEST_ASSERT_EQUAL_size_t(2u, BE.zb_devices(NULL, rows, 8u));
    /* the water sensor (a contact with a water zone type) is a water sensor */
    add_round();
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEVCLASS_WATER, page(HOST_ZBR_WATER).dev.device_class);
    TEST_ASSERT_EQUAL_UINT8(ALARM_DEVCLASS_SMOKE, page(HOST_ZBR_SMOKE).dev.device_class);
}

/* ZB-021, ZB-027: a rename is stored and read back by a restarted panel; refused outside installer
 * mode (NOT_ALLOWED), and nothing changes. */
static void test_rename_survives_a_restart(void)
{
    add_round();
    const uint16_t id = host_zigbee_real_id(HOST_ZBR_DOOR);
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_RENAME;
    req.user = 1u;
    req.id = id;
    snprintf(req.name, sizeof(req.name), "%s", "Porch Door");
    g_allowed = false;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_NOT_ALLOWED, BE.zb_request(NULL, &req));
    TEST_ASSERT_EQUAL_STRING("", page(HOST_ZBR_DOOR).dev.name);
    g_allowed = true;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, BE.zb_request(NULL, &req));
    TEST_ASSERT_EQUAL_STRING("Porch Door", page(HOST_ZBR_DOOR).dev.name);

    host_zigbee_real_restart(g_now);
    run(2000u);
    TEST_ASSERT_EQUAL_UINT16(id, host_zigbee_real_id(HOST_ZBR_DOOR));
    TEST_ASSERT_EQUAL_STRING("Porch Door", page(HOST_ZBR_DOOR).dev.name);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_DEV_READY, page(HOST_ZBR_DOOR).dev.state);
    TEST_ASSERT_EQUAL_UINT16(2u, status().devices);
}

/* ZB-012, ZSRC: a Zigbee device feeds a zone through the hub, as the zone editor binds it, and its
 * page names the zone. */
static void test_device_feeds_a_zone(void)
{
    add_round();
    const ui_zb_device_page_t pg = page(HOST_ZBR_DOOR);
    zone_hub_binding_t b;
    memset(&b, 0, sizeof(b));
    b.in[ZSRC_ROLE_MAIN].addr = pg.inputs[0].addr;
    zsrc_mapping_defaults(&b.in[ZSRC_ROLE_MAIN].map, ALARM_DEVCLASS_DOOR);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&HUB, 3u, &b, g_now));
    g_nzev = 0u;
    TEST_ASSERT_TRUE(host_zigbee_real_trip(HOST_ZBR_DOOR, true));
    run(100u);
    bool faulted = false;
    for (unsigned i = 0; i < g_nzev; i++) {
        faulted = faulted || (g_zev[i].zone == 3u && g_zev[i].ev == (uint8_t)ALARM_ZEV_FAULT);
    }
    TEST_ASSERT_TRUE_MESSAGE(faulted, "the door opening didn't fault zone 3");
    TEST_ASSERT_EQUAL_UINT16(3u, page(HOST_ZBR_DOOR).inputs[0].zone);
    TEST_ASSERT_EQUAL_STRING("open", page(HOST_ZBR_DOOR).inputs[0].state); /* the source's word */
    /* a device out of the network can't trip anything */
    TEST_ASSERT_FALSE(host_zigbee_real_trip(HOST_ZBR_SIREN, true));
}

/* ZB-013: Remove asks a sleeping device to leave; it answers when it wakes (its button), and the
 * page says Removed with the zones it fed; logged as the installer's. */
static void test_remove_answered_when_it_wakes(void)
{
    add_round();
    const uint16_t id = host_zigbee_real_id(HOST_ZBR_DOOR);
    run(60000u); /* asleep by now */
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_REMOVE, id));
    run(1000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_WORKING, progress().step);
    TEST_ASSERT_TRUE(host_zigbee_real_press(HOST_ZBR_DOOR)); /* its button wakes it */
    run(2000u);
    const ui_zb_progress_t g = progress();
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_OP_REMOVE, g.op);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, g.step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RES_REMOVED, g.result);
    TEST_ASSERT_EQUAL_UINT16(0u, host_zigbee_real_id(HOST_ZBR_DOOR));
    TEST_ASSERT_FALSE(host_zigbee_real_joined(HOST_ZBR_DOOR));
    ui_zb_device_page_t pg;
    TEST_ASSERT_FALSE(BE.zb_device(NULL, id, &pg));
    TEST_ASSERT_EQUAL_UINT(1u, logged(ZBZ_EV_REMOVED));
    TEST_ASSERT_EQUAL_UINT16(1u, status().devices);
    /* gone: the next add brings it back first */
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_DISMISS, 0u));
    add_round();
    TEST_ASSERT_TRUE(host_zigbee_real_joined(HOST_ZBR_DOOR));
}

/* ZB-015: an install code kept for later is listed, a typo refused (CHECKSUM), and it survives a
 * restart. */
static void test_install_code_added(void)
{
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_CODE_ADD;
    req.user = 1u;
    req.ieee = 0x02ABCD0000000002ull;
    snprintf(req.code, sizeof(req.code), "%s", "1122 3344 5566 5A61");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_CHECKSUM, BE.zb_request(NULL, &req));
    snprintf(req.code, sizeof(req.code), "%s", "1122 3344 5566 5A60");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, BE.zb_request(NULL, &req));
    ui_zb_code_t codes[4];
    TEST_ASSERT_EQUAL_size_t(1u, BE.zb_codes(NULL, codes, 4u));
    TEST_ASSERT_EQUAL_UINT64(0x02ABCD0000000002ull, codes[0].ieee);
    TEST_ASSERT_EQUAL_UINT32(1790000000u, codes[0].added_at);
    TEST_ASSERT_EQUAL_UINT16(1u, status().codes_waiting);
    host_zigbee_real_restart(g_now);
    run(1000u);
    TEST_ASSERT_EQUAL_size_t(1u, BE.zb_codes(NULL, codes, 4u));
}

/* ZB-023, ZB-024: the siren's and the keypad's settings - the siren sounds for partition 1, kept
 * across a restart; the keypad gets a device number from the panel, never one another part holds
 * (the panel's own keypad 1). */
static void test_siren_and_keypad_settings(void)
{
    add_round(); /* door, motion */
    add_round(); /* smoke, water */
    add_round(); /* siren, keypad */
    TEST_ASSERT_TRUE(host_zigbee_real_joined(HOST_ZBR_SIREN));
    TEST_ASSERT_TRUE(host_zigbee_real_joined(HOST_ZBR_KEYPAD));
    run(30000u);
    ui_zb_device_page_t sp = page(HOST_ZBR_SIREN);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_KIND_SIREN, sp.dev.kind);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_FIT_AVAILABLE, sp.siren.fit);
    TEST_ASSERT_EQUAL_UINT8(0u, sp.siren.partitions);

    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_SIREN;
    req.user = 1u;
    req.id = sp.dev.id;
    req.siren = sp.siren;
    req.siren.partitions = 1u;
    req.siren.burglary = true;
    req.siren.fire = true;
    req.siren.volume = 2u;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, BE.zb_request(NULL, &req));
    sp = page(HOST_ZBR_SIREN);
    TEST_ASSERT_EQUAL_UINT8(1u, sp.siren.partitions);
    TEST_ASSERT_TRUE(sp.siren.live);
    TEST_ASSERT_EQUAL_UINT8(2u, sp.siren.volume);
    /* "Test it": an operation that ends with its answer */
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_SIREN_TEST, sp.dev.id));
    run(15000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_STEP_DONE, progress().step);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_OP_SIREN_TEST, progress().op);
    TEST_ASSERT_NOT_EQUAL_UINT8(UI_ZB_RES_NONE, page(HOST_ZBR_SIREN).siren.last_test);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, ask(UI_ZB_REQ_DISMISS, 0u));

    ui_zb_device_page_t kp = page(HOST_ZBR_KEYPAD);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_KIND_KEYPAD, kp.dev.kind);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_FIT_AVAILABLE, kp.keypad.fit);
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_KEYPAD;
    req.user = 1u;
    req.id = kp.dev.id;
    req.keypad = kp.keypad;
    req.keypad.partition = 1u;
    req.keypad.show_state = true;
    req.keypad.device = 1u; /* ignored: the panel gives the number */
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, BE.zb_request(NULL, &req));
    kp = page(HOST_ZBR_KEYPAD);
    TEST_ASSERT_EQUAL_UINT8(1u, kp.keypad.partition);
    TEST_ASSERT_TRUE(kp.keypad.device >= 2u); /* never the panel's own keypad 1 */
    TEST_ASSERT_TRUE(kp.keypad.live);

    host_zigbee_real_restart(g_now);
    run(2000u);
    TEST_ASSERT_EQUAL_UINT8(1u, page(HOST_ZBR_SIREN).siren.partitions);
    TEST_ASSERT_EQUAL_UINT8(1u, page(HOST_ZBR_KEYPAD).keypad.partition);
    TEST_ASSERT_EQUAL_UINT8(kp.keypad.device, page(HOST_ZBR_KEYPAD).keypad.device);
}

/* ZB-017, ZB-027: the reset's installer code is checked first (WRONG_CODE, nothing reset); the
 * radio unplugged is said, and an add then refused NO_RADIO; back, it works again. */
static void test_refusals_and_the_radio(void)
{
    add_round();
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_RESET;
    req.user = 1u;
    snprintf(req.installer_code, sizeof(req.installer_code), "%s", "123456");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_WRONG_CODE, BE.zb_request(NULL, &req));
    TEST_ASSERT_EQUAL_UINT16(2u, status().devices);

    host_zigbee_real_unplug(true);
    run(1000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RADIO_NOT_CONNECTED, status().radio);
    TEST_ASSERT_EQUAL(UI_ZB_REQ_NO_RADIO, ask(UI_ZB_REQ_ADD, 0u));
    TEST_ASSERT_EQUAL(HOST_ZBR_COUNT, host_zigbee_real_press_next());
    host_zigbee_real_unplug(false);
    run(1000u);
    TEST_ASSERT_EQUAL_UINT8(UI_ZB_RADIO_WORKING, status().radio);
    TEST_ASSERT_EQUAL_UINT16(2u, status().devices);

    /* the reset with the installer's code: a new, empty network, logged */
    snprintf(req.installer_code, sizeof(req.installer_code), "%s", "69420");
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, BE.zb_request(NULL, &req));
    run(5000u);
    TEST_ASSERT_EQUAL_UINT16(0u, status().devices);
    TEST_ASSERT_EQUAL_UINT(1u, logged(ZBZ_EV_NETWORK_RESET));
}

/* ZB-015, ZB-027: the store keeps the largest codes value - ZBZ_CODES_MAX codes of 128 bits, more
 * than a registry record's size - across a restart, as the panel's NVS does. */
static void test_every_long_code_survives_a_restart(void)
{
    ui_zb_request_t req;
    for (unsigned i = 0; i < ZBZ_CODES_MAX; i++) {
        uint8_t b[ZBZ_CODE_MAX];
        for (unsigned k = 0; k < 16u; k++) {
            b[k] = (uint8_t)(0x10u * i + k);
        }
        const uint16_t crc = zbz_code_crc(b, 16u);
        b[16] = (uint8_t)(crc & 0xFFu);
        b[17] = (uint8_t)(crc >> 8);
        memset(&req, 0, sizeof(req));
        req.kind = UI_ZB_REQ_CODE_ADD;
        req.user = 1u;
        req.ieee = 0x02ABCD0000000100ull + i;
        size_t o = 0u;
        for (unsigned k = 0; k < ZBZ_CODE_MAX; k++) {
            o += (size_t)snprintf(req.code + o, sizeof(req.code) - o, "%02X", (unsigned)b[k]);
        }
        TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, BE.zb_request(NULL, &req));
    }
    run(1000u);
    TEST_ASSERT_EQUAL_UINT16(ZBZ_CODES_MAX, status().codes_waiting);
    host_zigbee_real_restart(g_now);
    run(1000u);
    ui_zb_code_t codes[ZBZ_CODES_MAX + 1u];
    TEST_ASSERT_EQUAL_size_t(ZBZ_CODES_MAX, BE.zb_codes(NULL, codes, ZBZ_CODES_MAX + 1u));
    bool last = false;
    for (unsigned i = 0; i < ZBZ_CODES_MAX; i++) {
        last = last || codes[i].ieee == 0x02ABCD0000000100ull + ZBZ_CODES_MAX - 1u;
    }
    TEST_ASSERT_TRUE_MESSAGE(last, "the last code was lost by the restart");
    TEST_ASSERT_EQUAL_UINT16(ZBZ_CODES_MAX, status().codes_waiting);
}

/* ZB-023, ZB-024: the engine-facing ports - a keypad's Arm reaches the engine (the alarm task's
 * part) with its code, and the engine's answer comes back to the keypad as its Arm Response (a
 * wrong code: invalid code); a siren silent past its supervision time is the bell trouble, cleared
 * by its next frame. */
static void test_keypad_arm_and_silent_siren_reach_the_engine(void)
{
    add_round(); /* door, motion */
    add_round(); /* smoke, water */
    add_round(); /* siren, keypad */
    run(30000u);
    ui_zb_device_page_t kp = page(HOST_ZBR_KEYPAD);
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_KEYPAD;
    req.user = 1u;
    req.id = kp.dev.id;
    req.keypad = kp.keypad;
    req.keypad.partition = 1u;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, BE.zb_request(NULL, &req));
    kp = page(HOST_ZBR_KEYPAD);
    TEST_ASSERT_TRUE(kp.keypad.live);

    TEST_ASSERT_EQUAL_UINT(0u, g_posts);
    const uint32_t before = host_zigbee_real_received(HOST_ZBR_KEYPAD, NULL);
    TEST_ASSERT_TRUE(host_zigbee_real_arm(HOST_ZBR_KEYPAD, ZBA_ARM_ALL, "1234"));
    run(500u);
    TEST_ASSERT_EQUAL_UINT(1u, g_posts);
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_ARM, g_post.cmd.type);
    TEST_ASSERT_EQUAL_UINT8(1u, g_post.cmd.partition);
    TEST_ASSERT_EQUAL_INT(ALARM_LEVEL_AWAY, g_post.cmd.level);
    TEST_ASSERT_EQUAL_STRING("1234", g_post.cmd.code);
    TEST_ASSERT_EQUAL_UINT8(0u, g_post.user); /* a keypad: its code, never a user */
    TEST_ASSERT_EQUAL_UINT8(kp.keypad.device, g_post.source.id);
    TEST_ASSERT_EQUAL_UINT(1u, host_zigbee_real_ace_answered());
    zsrc_zb_zcl_t got;
    TEST_ASSERT_TRUE(host_zigbee_real_received(HOST_ZBR_KEYPAD, &got) > before);
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZCL_IAS_ACE, got.cluster);
    TEST_ASSERT_EQUAL_HEX8(ZBA_RSP_ARM, got.command);
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_ALL_ARMED, got.payload[0]);

    /* a wrong code: the engine refuses it, the keypad hears so */
    g_ace_answer = ALARM_ERR_INVALID_CODE;
    run(5000u);
    TEST_ASSERT_TRUE(host_zigbee_real_arm(HOST_ZBR_KEYPAD, ZBA_ARM_ALL, "9999"));
    run(500u);
    TEST_ASSERT_EQUAL_UINT(2u, g_posts);
    TEST_ASSERT_EQUAL_STRING("9999", g_post.cmd.code);
    TEST_ASSERT_EQUAL_UINT(2u, host_zigbee_real_ace_answered());
    (void)host_zigbee_real_received(HOST_ZBR_KEYPAD, &got);
    TEST_ASSERT_EQUAL_HEX8(ZBA_RSP_ARM, got.command);
    TEST_ASSERT_EQUAL_UINT8(ZBA_NOTE_INVALID_CODE, got.payload[0]);
    /* a sensor has no IAS ACE: nothing sent */
    TEST_ASSERT_FALSE(host_zigbee_real_arm(HOST_ZBR_DOOR, ZBA_ARM_ALL, "1234"));

    /* the siren on partition 1, supervised for the shortest time the installer may set */
    ui_zb_device_page_t sp = page(HOST_ZBR_SIREN);
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_SIREN;
    req.user = 1u;
    req.id = sp.dev.id;
    req.siren = sp.siren;
    req.siren.partitions = 1u;
    req.siren.burglary = true;
    req.siren.supervision_min = ZBW_SUPERVISION_MIN_MIN;
    TEST_ASSERT_EQUAL(UI_ZB_REQ_OK, BE.zb_request(NULL, &req));
    run(1000u);
    TEST_ASSERT_TRUE(g_bell != 1);
    /* ZB-023: the panel reads a quiet siren before its limit, so one that answers stays clear... */
    run((uint64_t)ZBW_SUPERVISION_MIN_MIN * 60000u * 2u);
    TEST_ASSERT_TRUE(g_bell != 1);
    /* ... and one that answers nothing goes silent */
    TEST_ASSERT_TRUE(host_zigbee_real_mute(HOST_ZBR_SIREN, true));
    run((uint64_t)ZBW_SUPERVISION_MIN_MIN * 60000u + 1000u);
    TEST_ASSERT_EQUAL_INT(1, g_bell);
    TEST_ASSERT_TRUE(host_zigbee_real_mute(HOST_ZBR_SIREN, false));
    TEST_ASSERT_TRUE(host_zigbee_real_press(HOST_ZBR_SIREN)); /* a frame from it */
    run(1000u);
    TEST_ASSERT_EQUAL_INT(0, g_bell);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_add_two_join_and_reach_ready);
    RUN_TEST(test_device_page_words_data);
    RUN_TEST(test_rename_survives_a_restart);
    RUN_TEST(test_device_feeds_a_zone);
    RUN_TEST(test_remove_answered_when_it_wakes);
    RUN_TEST(test_install_code_added);
    RUN_TEST(test_siren_and_keypad_settings);
    RUN_TEST(test_refusals_and_the_radio);
    RUN_TEST(test_every_long_code_survives_a_restart);
    RUN_TEST(test_keypad_arm_and_silent_siren_reach_the_engine);
    RUN_TEST(test_stopped_setup_ends_not_finished);
    return UNITY_END();
}
