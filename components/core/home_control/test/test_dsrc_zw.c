/*
 * test_dsrc_zw.c - the panel's own Z-Wave outputs as a device source (components/core/home_control
 * dsrc_zw, docs/features/41 DESIGN section 4 "Z-Wave (phase B)", BL-169 B1): listing and kinds,
 * state words, every kind x action allowed or refused (a garage door is closed with 0 and opened
 * with 100 - the output layer presses only when its sensor says the door is the other way - and
 * never stopped, positioned or toggled),
 * the posted command's request id coming back as a result, offline and unknown keys, a new list
 * replacing the old, the registration order against a full Home Assistant list, and the
 * automation path through home_control.
 * Requirements: HCTL-001, HCTL-011, AUT-002, AUT-015, HCTL-012
 */
#include <stdio.h>
#include <string.h>

#include "home_control/dsrc_ha.h"
#include "home_control/dsrc_zw.h"
#include "home_control/home_control.h"
#include "unity.h"

#define K_PLUG   "zw_0a1b2c3d_7_2_0"
#define K_DIMMER "zw_0a1b2c3d_8_1_0"
#define K_GARAGE "zw_0a1b2c3d_9_1_2"

typedef struct {
    unsigned calls;
    char key[DSRC_ZW_KEY_LEN];
    int level;
    uint32_t request_id;
    bool returns;
} zsend_t;

static hc_t HC;
static dsrc_zw_t ZW;
static dsrc_ha_t HA;
static zsend_t SEND;

static unsigned SINK_CHANGES; /* through hc's sink to the port */
static unsigned RESULTS;
static uint32_t R_ID;
static bool R_OK;
static char R_REF[DEV_REF_LEN];
static char R_ERR[HC_ERROR_LEN];

static void port_changed(void *ctx)
{
    (void)ctx;
    SINK_CHANGES++;
}

static void port_result(void *ctx, uint32_t request_id, const char *ref, bool ok,
                        const char *error)
{
    (void)ctx;
    RESULTS++;
    R_ID = request_id;
    R_OK = ok;
    snprintf(R_REF, sizeof(R_REF), "%s", ref != NULL ? ref : "");
    snprintf(R_ERR, sizeof(R_ERR), "%s", error != NULL ? error : "");
}

static bool zsend(void *ctx, const char *key, int level, uint32_t request_id)
{
    zsend_t *s = ctx;
    s->calls++;
    snprintf(s->key, sizeof(s->key), "%s", key);
    s->level = level;
    s->request_id = request_id;
    return s->returns;
}

void setUp(void)
{
    const hc_port_t port = {.ctx = NULL, .changed = port_changed, .result = port_result};
    hc_init(&HC, &port);
    dsrc_zw_init(&ZW, hc_sink(&HC));
    dsrc_ha_init(&HA, hc_sink(&HC));
    memset(&SEND, 0, sizeof(SEND));
    SEND.returns = true;
    SEND.level = -99;
    dsrc_zw_set_sender(&ZW, zsend, &SEND);
    SINK_CHANGES = 0;
    RESULTS = 0;
    R_ID = 0;
    R_OK = false;
    R_REF[0] = '\0';
    R_ERR[0] = '\0';
}

void tearDown(void) {}

/* The three outputs, the network up. */
static void feed_three(void)
{
    dsrc_zw_link(&ZW, true);
    dsrc_zw_begin(&ZW);
    dsrc_zw_add(&ZW, K_PLUG, DSRC_ZW_SWITCH, "Lamp plug", 100, true, "", 0u);
    dsrc_zw_add(&ZW, K_DIMMER, DSRC_ZW_LIGHT, "Hall dimmer", 40, true, "", 0u);
    dsrc_zw_add(&ZW, K_GARAGE, DSRC_ZW_GARAGE, "Garage", 0, true, "closed", 0u);
    dsrc_zw_end(&ZW);
}

static const dev_info_t *listed(const char *ref, dev_info_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(buf[i].ref, ref) == 0) {
            return &buf[i];
        }
    }
    return NULL;
}

static size_t zw_list(dev_info_t *buf, size_t max)
{
    return dsrc_zw_ops()->list(&ZW, buf, max);
}

static bool zw_act(const char *ref, dev_action_t a, int32_t value, uint32_t id)
{
    return dsrc_zw_ops()->act(&ZW, ref, a, value, NULL, id);
}

/* ---- listing --------------------------------------------------------------------------------- */

/* HCTL-001: the outputs become devices: kinds, words, refs = keys, no area, the source stamped. */
static void test_list_and_kinds(void)
{
    static dev_info_t buf[DSRC_ZW_MAX];
    const device_source_ops_t *ops = dsrc_zw_ops();
    TEST_ASSERT_EQUAL(DSRC_ZWAVE, ops->kind);
    TEST_ASSERT_EQUAL(3, DSRC_ZWAVE); /* appended: stored in automations, never renumbered */
    TEST_ASSERT_EQUAL_STRING("zwave", ops->name);
    TEST_ASSERT_FALSE(ops->online(&ZW));

    dsrc_zw_link(&ZW, true);
    TEST_ASSERT_TRUE(ops->online(&ZW));
    const unsigned before = SINK_CHANGES;
    dsrc_zw_begin(&ZW);
    dsrc_zw_add(&ZW, K_PLUG, DSRC_ZW_SWITCH, "Lamp plug", 100, true, "", 0u);
    dsrc_zw_add(&ZW, K_DIMMER, DSRC_ZW_LIGHT, "", 40, true, "", 0u);
    dsrc_zw_add(&ZW, K_GARAGE, DSRC_ZW_GARAGE, "Garage", -1, true, "closed", 0u);
    dsrc_zw_add(&ZW, "zw_0a1b2c3d_10_1_0", 9, "Thermostat", 0, true, "", 0u); /* unknown kind */
    dsrc_zw_add(&ZW, "zw_0a1b2c3d_11_1_0", 0, "None", 0, true, "", 0u);
    dsrc_zw_add(&ZW, "", DSRC_ZW_SWITCH, "No key", 0, true, "", 0u);
    dsrc_zw_add(&ZW, NULL, DSRC_ZW_SWITCH, "No key", 0, true, "", 0u);
    /* a key that doesn't fit is refused, never cut (two cut keys could match) */
    dsrc_zw_add(&ZW, "zw_0123456789abcdef0123456789abcdef_1_1_1", DSRC_ZW_SWITCH, "Long", 0,
                true, "", 0u);
    TEST_ASSERT_EQUAL_size_t(0, zw_list(buf, DSRC_ZW_MAX)); /* nothing until end */
    TEST_ASSERT_EQUAL_UINT(before, SINK_CHANGES);
    dsrc_zw_end(&ZW);
    TEST_ASSERT_EQUAL_UINT(before + 1u, SINK_CHANGES);

    TEST_ASSERT_EQUAL_size_t(3, zw_list(buf, DSRC_ZW_MAX));
    const dev_info_t *p = &buf[0];
    TEST_ASSERT_EQUAL_STRING(K_PLUG, p->ref);
    TEST_ASSERT_EQUAL(DSRC_ZWAVE, p->source);
    TEST_ASSERT_EQUAL(DEV_SWITCH, p->kind);
    TEST_ASSERT_EQUAL_STRING("Lamp plug", p->name);
    TEST_ASSERT_EQUAL_STRING("", p->area);
    TEST_ASSERT_EQUAL_STRING("on", p->state);
    TEST_ASSERT_TRUE(p->online);
    TEST_ASSERT_EQUAL(DEV_LIGHT, buf[1].kind);
    TEST_ASSERT_EQUAL_STRING(K_DIMMER, buf[1].name); /* no name: the key */
    TEST_ASSERT_EQUAL_STRING("on", buf[1].state);   /* a dimmer at 40 is on, as HA says */
    TEST_ASSERT_EQUAL(DEV_COVER, buf[2].kind);
    TEST_ASSERT_EQUAL_STRING("closed", buf[2].state); /* the sensor's word, not the level */

    /* a short buffer gets a prefix */
    TEST_ASSERT_EQUAL_size_t(2, zw_list(buf, 2));

    /* through the registry: listed with the source */
    hc_register(&HC, ops, &ZW);
    hc_refresh(&HC);
    TEST_ASSERT_EQUAL_size_t(3, HC.n_devices);
    TEST_ASSERT_EQUAL(DSRC_ZWAVE, hc_get(&HC, K_GARAGE)->info.source);
}

/* HCTL-001: the outputs' states, the link and the devices' own online. */
static void test_states_and_link(void)
{
    static dev_info_t buf[DSRC_ZW_MAX];
    feed_three();
    unsigned c = SINK_CHANGES;

    dsrc_zw_state(&ZW, K_DIMMER, 0, true, "");
    TEST_ASSERT_EQUAL_UINT(++c, SINK_CHANGES);
    zw_list(buf, DSRC_ZW_MAX);
    TEST_ASSERT_EQUAL_STRING("off", listed(K_DIMMER, buf, 3)->state);

    dsrc_zw_state(&ZW, K_DIMMER, 0, true, ""); /* nothing changed: nothing told */
    TEST_ASSERT_EQUAL_UINT(c, SINK_CHANGES);

    dsrc_zw_state(&ZW, K_DIMMER, -1, true, "");
    zw_list(buf, DSRC_ZW_MAX);
    TEST_ASSERT_EQUAL_STRING("unknown", listed(K_DIMMER, buf, 3)->state);
    dsrc_zw_state(&ZW, K_PLUG, 0, true, "");
    dsrc_zw_state(&ZW, K_GARAGE, 0, true, "open");
    dsrc_zw_state(&ZW, K_DIMMER, 75, false, ""); /* the node itself is unreachable */
    zw_list(buf, DSRC_ZW_MAX);
    TEST_ASSERT_EQUAL_STRING("off", listed(K_PLUG, buf, 3)->state);
    TEST_ASSERT_EQUAL_STRING("open", listed(K_GARAGE, buf, 3)->state);
    TEST_ASSERT_EQUAL_STRING("on", listed(K_DIMMER, buf, 3)->state);
    TEST_ASSERT_FALSE(listed(K_DIMMER, buf, 3)->online);
    dsrc_zw_state(&ZW, K_GARAGE, 0, true, ""); /* no word: unknown, never a guess */
    zw_list(buf, DSRC_ZW_MAX);
    TEST_ASSERT_EQUAL_STRING("unknown", listed(K_GARAGE, buf, 3)->state);

    c = SINK_CHANGES;
    dsrc_zw_state(&ZW, "zw_0a1b2c3d_99_1_0", 100, true, ""); /* not listed: ignored */
    dsrc_zw_state(&ZW, NULL, 100, true, "");
    TEST_ASSERT_EQUAL_UINT(c, SINK_CHANGES);

    /* the network drops: everything greys, the source says offline */
    dsrc_zw_link(&ZW, false);
    TEST_ASSERT_EQUAL_UINT(c + 1u, SINK_CHANGES);
    TEST_ASSERT_FALSE(dsrc_zw_ops()->online(&ZW));
    zw_list(buf, DSRC_ZW_MAX);
    for (size_t i = 0; i < 3; i++) {
        TEST_ASSERT_FALSE(buf[i].online);
    }
    /* a state while it is down can't bring a device online */
    dsrc_zw_state(&ZW, K_PLUG, 100, true, "");
    zw_list(buf, DSRC_ZW_MAX);
    TEST_ASSERT_FALSE(listed(K_PLUG, buf, 3)->online);
    /* back up: online again with its next state */
    dsrc_zw_link(&ZW, true);
    dsrc_zw_state(&ZW, K_PLUG, 100, true, "");
    zw_list(buf, DSRC_ZW_MAX);
    TEST_ASSERT_TRUE(listed(K_PLUG, buf, 3)->online);
}

/* HCTL-001: begin..end replaces the list; the old one is listed until end; bounded. */
static void test_begin_end_replaces_the_list(void)
{
    static dev_info_t buf[DSRC_ZW_MAX + 4u];
    feed_three();
    dsrc_zw_begin(&ZW);
    dsrc_zw_add(&ZW, "zw_0a1b2c3d_20_1_0", DSRC_ZW_SWITCH, "Fountain", 0, true, "", 0u);
    dsrc_zw_add(&ZW, "zw_0a1b2c3d_20_1_0", DSRC_ZW_SWITCH, "Fountain pump", 100, true, "",
                0u); /* again */
    dsrc_zw_state(&ZW, "zw_0a1b2c3d_20_1_0", 0, true, ""); /* reaches the list being built */
    TEST_ASSERT_EQUAL_size_t(3, zw_list(buf, DSRC_ZW_MAX)); /* a refresh now: the old list */
    TEST_ASSERT_NOT_NULL(listed(K_PLUG, buf, 3));
    dsrc_zw_end(&ZW);
    TEST_ASSERT_EQUAL_size_t(1, zw_list(buf, DSRC_ZW_MAX));
    TEST_ASSERT_EQUAL_STRING("Fountain pump", buf[0].name);
    TEST_ASSERT_EQUAL_STRING("off", buf[0].state);

    /* an end or an add with no begin changes nothing */
    const unsigned c = SINK_CHANGES;
    dsrc_zw_add(&ZW, K_PLUG, DSRC_ZW_SWITCH, "Lamp plug", 0, true, "", 0u);
    dsrc_zw_end(&ZW);
    TEST_ASSERT_EQUAL_UINT(c, SINK_CHANGES);
    TEST_ASSERT_EQUAL_size_t(1, zw_list(buf, DSRC_ZW_MAX));

    /* never more than the outputs' own bound */
    dsrc_zw_begin(&ZW);
    for (unsigned i = 0; i < DSRC_ZW_MAX + 4u; i++) {
        char key[DSRC_ZW_KEY_LEN];
        snprintf(key, sizeof(key), "zw_0a1b2c3d_%u_1_0", i + 1u);
        dsrc_zw_add(&ZW, key, DSRC_ZW_SWITCH, "", 0, true, "", 0u);
    }
    dsrc_zw_end(&ZW);
    TEST_ASSERT_EQUAL_size_t(DSRC_ZW_MAX, zw_list(buf, DSRC_ZW_MAX + 4u));
    TEST_ASSERT_EQUAL(18, DSRC_ZW_MAX); /* ZWO_OUTPUTS 16 + ZWO_GARAGES 2 */

    /* an empty list empties it */
    dsrc_zw_begin(&ZW);
    dsrc_zw_end(&ZW);
    TEST_ASSERT_EQUAL_size_t(0, zw_list(buf, DSRC_ZW_MAX));
}

/* ---- actions --------------------------------------------------------------------------------- */

/* AUT-002 (layer 6): every kind x action at the source itself. Allowed pairs post one level (a
 * garage's CLOSE 0, OPEN 100); every other pair - a garage's STOP and any position, any TOGGLE -
 * is refused with nothing posted, for every caller: a press can't do them. */
static void test_act_every_kind_and_action(void)
{
    static const struct {
        const char *ref;
        dev_kind_t kind;
    } devs[] = {{K_PLUG, DEV_SWITCH}, {K_DIMMER, DEV_LIGHT}, {K_GARAGE, DEV_COVER}};
    static const int32_t values[] = {-2, -1, 0, 40, 100, 101};
    feed_three();
    for (size_t k = 0; k < 3; k++) {
        for (int a = ACT_NONE; a < ACT_COUNT; a++) {
            for (size_t v = 0; v < sizeof(values) / sizeof(values[0]); v++) {
                const int32_t val = values[v];
                int want = -1;
                switch (devs[k].kind) {
                case DEV_SWITCH:
                    want = a == ACT_ON ? 100 : a == ACT_OFF ? 0 : -1;
                    break;
                case DEV_LIGHT:
                    if (a == ACT_OFF) {
                        want = 0;
                    } else if (a == ACT_ON && val == -1) {
                        want = 100; /* no "previous level" form on Z-Wave outputs */
                    } else if ((a == ACT_ON || a == ACT_SET_LEVEL) && val >= 0 && val <= 100) {
                        want = (int)val;
                    }
                    break;
                default:
                    want = a == ACT_CLOSE ? 0 : a == ACT_OPEN ? 100 : -1;
                    break;
                }
                char msg[64];
                snprintf(msg, sizeof(msg), "kind %d action %d value %d", (int)devs[k].kind, a,
                         (int)val);
                TEST_ASSERT_EQUAL_INT_MESSAGE(
                    want, dsrc_zw_level_for(devs[k].kind, (dev_action_t)a, val), msg);
                const unsigned calls = SEND.calls;
                const bool ok = zw_act(devs[k].ref, (dev_action_t)a, val, 77u);
                TEST_ASSERT_EQUAL_MESSAGE(want >= 0, ok, msg);
                if (want >= 0) {
                    TEST_ASSERT_EQUAL_UINT_MESSAGE(calls + 1u, SEND.calls, msg);
                    TEST_ASSERT_EQUAL_INT_MESSAGE(want, SEND.level, msg);
                    TEST_ASSERT_EQUAL_STRING_MESSAGE(devs[k].ref, SEND.key, msg);
                    TEST_ASSERT_EQUAL_UINT32_MESSAGE(77u, SEND.request_id, msg);
                } else {
                    TEST_ASSERT_EQUAL_UINT_MESSAGE(calls, SEND.calls, msg); /* nothing posted */
                }
            }
        }
    }
    /* the garage, spelled out: stop, a position, a toggle - never posted */
    const unsigned calls = SEND.calls;
    TEST_ASSERT_FALSE(zw_act(K_GARAGE, ACT_STOP, 0, 2u));
    TEST_ASSERT_FALSE(zw_act(K_GARAGE, ACT_SET_LEVEL, 100, 3u));
    TEST_ASSERT_FALSE(zw_act(K_GARAGE, ACT_SET_LEVEL, 0, 4u));
    TEST_ASSERT_FALSE(zw_act(K_GARAGE, ACT_TOGGLE, 0, 5u));
    TEST_ASSERT_EQUAL_UINT(calls, SEND.calls);
    TEST_ASSERT_TRUE(zw_act(K_GARAGE, ACT_CLOSE, 0, 6u));
    TEST_ASSERT_EQUAL_INT(0, SEND.level);
    TEST_ASSERT_TRUE(zw_act(K_GARAGE, ACT_OPEN, 0, 7u));
    TEST_ASSERT_EQUAL_INT(100, SEND.level);
    TEST_ASSERT_EQUAL_UINT(calls + 2u, SEND.calls);
    /* kinds this source never lists */
    TEST_ASSERT_EQUAL_INT(-1, dsrc_zw_level_for(DEV_LOCK, ACT_LOCK, 0));
    TEST_ASSERT_EQUAL_INT(-1, dsrc_zw_level_for(DEV_VALVE, ACT_CLOSE, 0));
}

/* AUT-006: offline and unknown keys fail at once, nothing posted - a Z-Wave send is never held. */
static void test_act_offline_and_unknown(void)
{
    feed_three();
    TEST_ASSERT_FALSE(zw_act("zw_0a1b2c3d_99_1_0", ACT_ON, -1, 1u));
    TEST_ASSERT_FALSE(zw_act(NULL, ACT_ON, -1, 1u));
    TEST_ASSERT_FALSE(zw_act("", ACT_ON, -1, 1u));
    dsrc_zw_state(&ZW, K_PLUG, 100, false, ""); /* the node is unreachable */
    TEST_ASSERT_FALSE(zw_act(K_PLUG, ACT_OFF, 0, 1u));
    TEST_ASSERT_EQUAL_UINT(0, SEND.calls);

    dsrc_zw_link(&ZW, false);
    TEST_ASSERT_FALSE(zw_act(K_DIMMER, ACT_OFF, 0, 1u));
    TEST_ASSERT_EQUAL_UINT(0, SEND.calls);

    dsrc_zw_link(&ZW, true);
    dsrc_zw_state(&ZW, K_DIMMER, 0, true, "");
    dsrc_zw_set_sender(&ZW, NULL, NULL); /* no platform yet */
    TEST_ASSERT_FALSE(zw_act(K_DIMMER, ACT_OFF, 0, 1u));

    /* the post refused (queue full): false, as the platform said */
    dsrc_zw_set_sender(&ZW, zsend, &SEND);
    SEND.returns = false;
    TEST_ASSERT_FALSE(zw_act(K_DIMMER, ACT_OFF, 0, 1u));
    TEST_ASSERT_EQUAL_UINT(1, SEND.calls);

    /* through home_control: unknown, offline, and a refused post leaving the device free */
    hc_register(&HC, dsrc_zw_ops(), &ZW);
    hc_refresh(&HC);
    TEST_ASSERT_EQUAL(HC_ERR_UNKNOWN, hc_act(&HC, "zw_0a1b2c3d_99_1_0", ACT_ON, -1, NULL));
    TEST_ASSERT_EQUAL(HC_ERR_OFFLINE, hc_act(&HC, K_DIMMER, ACT_OFF, 0, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, hc_get(&HC, K_DIMMER)->pending);
    TEST_ASSERT_EQUAL(HC_ERR_OFFLINE, hc_act(&HC, K_PLUG, ACT_OFF, 0, NULL)); /* node down */
    SEND.returns = true;
    dsrc_zw_link(&ZW, false);
    hc_refresh(&HC);
    TEST_ASSERT_EQUAL(HC_ERR_OFFLINE, hc_act(&HC, K_DIMMER, ACT_OFF, 0, NULL));
    TEST_ASSERT_EQUAL_UINT(2, SEND.calls);
}

/* HCTL-012: the posted request id comes back through dsrc_zw_result to home_control's port,
 * success and failure alike. */
static void test_request_id_round_trip(void)
{
    feed_three();
    hc_register(&HC, dsrc_zw_ops(), &ZW);
    hc_refresh(&HC);

    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, K_DIMMER, ACT_SET_LEVEL, 30, NULL));
    const uint32_t id = SEND.request_id;
    TEST_ASSERT_NOT_EQUAL(0, id);
    TEST_ASSERT_EQUAL_INT(30, SEND.level);
    TEST_ASSERT_EQUAL_UINT32(id, hc_get(&HC, K_DIMMER)->pending);
    TEST_ASSERT_EQUAL(HC_ERR_BUSY, hc_act(&HC, K_DIMMER, ACT_OFF, 0, NULL));

    dsrc_zw_result(&ZW, 0u, true, ""); /* no id: nothing */
    dsrc_zw_result(&ZW, id + 100u, true, ""); /* someone else's: nothing */
    TEST_ASSERT_EQUAL_UINT(0, RESULTS);
    dsrc_zw_result(&ZW, id, true, NULL);
    TEST_ASSERT_EQUAL_UINT(1, RESULTS);
    TEST_ASSERT_EQUAL_UINT32(id, R_ID);
    TEST_ASSERT_TRUE(R_OK);
    TEST_ASSERT_EQUAL_STRING(K_DIMMER, R_REF);
    TEST_ASSERT_EQUAL_STRING("", R_ERR);
    TEST_ASSERT_EQUAL_UINT32(0, hc_get(&HC, K_DIMMER)->pending);
    dsrc_zw_result(&ZW, id, true, ""); /* a duplicate: heard once */
    TEST_ASSERT_EQUAL_UINT(1, RESULTS);

    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, K_PLUG, ACT_ON, -1, NULL));
    const uint32_t id2 = SEND.request_id;
    TEST_ASSERT_NOT_EQUAL(id, id2);
    dsrc_zw_result(&ZW, id2, false, "timeout");
    TEST_ASSERT_EQUAL_UINT(2, RESULTS);
    TEST_ASSERT_EQUAL_UINT32(id2, R_ID);
    TEST_ASSERT_FALSE(R_OK);
    TEST_ASSERT_EQUAL_STRING("timeout", R_ERR);
    TEST_ASSERT_TRUE(hc_get(&HC, K_PLUG)->failed);
    TEST_ASSERT_EQUAL_STRING("timeout", hc_get(&HC, K_PLUG)->error);
    /* HCTL-012: sent, and the device didn't confirm (ZWO_ERR_TIMEOUT): it may have happened */
    TEST_ASSERT_TRUE(hc_outcome_unknown(hc_get(&HC, K_PLUG)));

    /* a failure with no word reads "failed" */
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, K_PLUG, ACT_OFF, 0, NULL));
    dsrc_zw_result(&ZW, SEND.request_id, false, NULL);
    TEST_ASSERT_EQUAL_STRING("failed", R_ERR);
}

/* HCTL-012 (BL-171): the Z-Wave output layer's refusals come back through dsrc_zw_result - its
 * "offline" (said before anything is tried), "send_failed" (the command couldn't be queued) and
 * "busy" - and are definite: the screen says "didn't". Unknown are a command the layer sent and
 * the device didn't confirm (its "timeout", test_request_id_round_trip) and home_control's own
 * failures of a command in flight: no answer by the deadline, or the Z-Wave network dropping
 * under it (the registry's "offline", the same word as the layer's). */
static void test_outcome_by_origin(void)
{
    feed_three();
    hc_register(&HC, dsrc_zw_ops(), &ZW);
    hc_refresh(&HC);

    static const char *const REFUSALS[] = {"offline", "send_failed", "busy", "failed"};
    for (size_t i = 0; i < sizeof REFUSALS / sizeof REFUSALS[0]; i++) {
        TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, K_PLUG, ACT_ON, -1, NULL));
        dsrc_zw_result(&ZW, SEND.request_id, false, REFUSALS[i]);
        const hc_device_t *d = hc_get(&HC, K_PLUG);
        TEST_ASSERT_TRUE(d->failed);
        TEST_ASSERT_EQUAL_STRING(REFUSALS[i], d->error);
        TEST_ASSERT_FALSE_MESSAGE(hc_outcome_unknown(d), REFUSALS[i]);
    }

    /* no answer by the deadline: unknown */
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, K_DIMMER, ACT_SET_LEVEL, 30, NULL));
    hc_tick(&HC, 1);
    hc_tick(&HC, 1u + HC_ACT_TIMEOUT_MS);
    TEST_ASSERT_EQUAL_STRING("timeout", hc_get(&HC, K_DIMMER)->error);
    TEST_ASSERT_TRUE(hc_outcome_unknown(hc_get(&HC, K_DIMMER)));

    /* the network drops with the plug's command in flight: "offline" too, but unknown - and
     * still unknown once it is back with the same report */
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, K_PLUG, ACT_OFF, 0, NULL));
    dsrc_zw_link(&ZW, false);
    hc_refresh(&HC);
    TEST_ASSERT_TRUE(hc_get(&HC, K_PLUG)->failed);
    TEST_ASSERT_EQUAL_STRING("offline", hc_get(&HC, K_PLUG)->error);
    TEST_ASSERT_TRUE(hc_outcome_unknown(hc_get(&HC, K_PLUG)));
    dsrc_zw_link(&ZW, true);
    hc_refresh(&HC);
    TEST_ASSERT_TRUE(hc_outcome_unknown(hc_get(&HC, K_PLUG)));
}

/* HCTL-012, AUT-009 (review 2): a garage door's whole move outlasts home_control's 30 s, so its
 * device carries its own wait (dsrc_zw_add's act_ms: zwo_garage_budget_ms, 232 s at the longest
 * travel). A slow door whose sensor confirms the close 40 s on ends done - no false "timeout",
 * no automation_failed; one whose end never comes is still "timeout" (unknown) at that wait, not
 * later; a plain output keeps HC_ACT_TIMEOUT_MS. */
#define GARAGE_WAIT_MS 232000u
static void test_garage_waits_its_whole_move(void)
{
    dsrc_zw_link(&ZW, true);
    dsrc_zw_begin(&ZW);
    dsrc_zw_add(&ZW, K_PLUG, DSRC_ZW_SWITCH, "Lamp plug", 100, true, "", 0u);
    dsrc_zw_add(&ZW, K_GARAGE, DSRC_ZW_GARAGE, "Garage", 0, true, "open", GARAGE_WAIT_MS);
    dsrc_zw_end(&ZW);
    hc_register(&HC, dsrc_zw_ops(), &ZW);
    hc_refresh(&HC);
    TEST_ASSERT_EQUAL_UINT32(GARAGE_WAIT_MS, hc_get(&HC, K_GARAGE)->info.act_ms);
    TEST_ASSERT_EQUAL_UINT32(0u, hc_get(&HC, K_PLUG)->info.act_ms);

    uint32_t id = 0;
    TEST_ASSERT_EQUAL(HC_OK, hc_act_auto(&HC, K_GARAGE, ACT_CLOSE, 0, NULL, false, &id));
    hc_tick(&HC, 1000u);
    hc_tick(&HC, 1000u + HC_ACT_TIMEOUT_MS); /* home_control's own 30 s: not this one's */
    hc_tick(&HC, 1000u + 40000u);
    TEST_ASSERT_EQUAL_UINT(0, RESULTS);
    TEST_ASSERT_EQUAL_UINT32(id, hc_get(&HC, K_GARAGE)->pending);
    dsrc_zw_result(&ZW, id, true, ""); /* its sensor: closed, 40 s on */
    TEST_ASSERT_EQUAL_UINT(1, RESULTS);
    TEST_ASSERT_EQUAL_UINT32(id, R_ID);
    TEST_ASSERT_TRUE(R_OK);
    TEST_ASSERT_FALSE(hc_get(&HC, K_GARAGE)->failed);

    /* a move whose end never comes: "timeout" at its own wait, no later */
    TEST_ASSERT_EQUAL(HC_OK, hc_act_auto(&HC, K_GARAGE, ACT_CLOSE, 0, NULL, false, &id));
    hc_tick(&HC, 100000u);
    hc_tick(&HC, 100000u + GARAGE_WAIT_MS - 1u);
    TEST_ASSERT_EQUAL_UINT(1, RESULTS);
    hc_tick(&HC, 100000u + GARAGE_WAIT_MS);
    TEST_ASSERT_EQUAL_UINT(2, RESULTS);
    TEST_ASSERT_EQUAL_UINT32(id, R_ID);
    TEST_ASSERT_FALSE(R_OK);
    TEST_ASSERT_EQUAL_STRING("timeout", R_ERR);
    TEST_ASSERT_TRUE(hc_outcome_unknown(hc_get(&HC, K_GARAGE)));

    /* the plug keeps home_control's own 30 s */
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, K_PLUG, ACT_OFF, 0, NULL));
    hc_tick(&HC, 400000u);
    hc_tick(&HC, 400000u + HC_ACT_TIMEOUT_MS);
    TEST_ASSERT_EQUAL_UINT(3, RESULTS);
    TEST_ASSERT_EQUAL_STRING(K_PLUG, R_REF);
    TEST_ASSERT_EQUAL_STRING("timeout", R_ERR);
}

/* AUT-002 / AUT-015 through home_control: an automation's light ON reaches the sender; a
 * garage's OPEN is refused by hc_auto_allowed with the installer option off and reaches the
 * sender as 100 with it on; positions and toggles never pass; CLOSE reaches the sender as 0; a
 * person's STOP (hc_act) passes the registry and the source still refuses it. */
static void test_automation_through_home_control(void)
{
    feed_three();
    hc_register(&HC, dsrc_zw_ops(), &ZW);
    hc_refresh(&HC);
    uint32_t id = 0;

    TEST_ASSERT_EQUAL(HC_OK, hc_act_auto(&HC, K_DIMMER, ACT_ON, -1, NULL, false, &id));
    TEST_ASSERT_NOT_EQUAL(0, id);
    TEST_ASSERT_EQUAL_UINT32(id, SEND.request_id);
    TEST_ASSERT_EQUAL_INT(100, SEND.level);
    TEST_ASSERT_EQUAL_STRING(K_DIMMER, SEND.key);
    dsrc_zw_result(&ZW, id, true, "");

    const unsigned calls = SEND.calls;
    TEST_ASSERT_EQUAL(HC_ERR_NOT_ALLOWED,
                      hc_act_auto(&HC, K_GARAGE, ACT_OPEN, 0, NULL, false, &id));
    TEST_ASSERT_EQUAL_UINT32(0, id);
    TEST_ASSERT_EQUAL(HC_ERR_NOT_ALLOWED,
                      hc_act_auto(&HC, K_GARAGE, ACT_SET_LEVEL, 0, NULL, true, &id));
    TEST_ASSERT_EQUAL(HC_ERR_NOT_ALLOWED,
                      hc_act_auto(&HC, K_DIMMER, ACT_SET_LEVEL, 50, NULL, true, &id));
    TEST_ASSERT_EQUAL_UINT(calls, SEND.calls);
    TEST_ASSERT_EQUAL_UINT32(0, hc_get(&HC, K_GARAGE)->pending);

    /* a toggle passes the registry (a switch may be toggled) and the source refuses it */
    TEST_ASSERT_EQUAL(HC_ERR_OFFLINE, hc_act_auto(&HC, K_PLUG, ACT_TOGGLE, 0, NULL, true, &id));
    TEST_ASSERT_EQUAL_UINT32(0, id);
    /* a person's stop (code checked by the platform) passes the registry; a press can't stop a
     * door, so the source refuses it - nothing posted, the device left free */
    TEST_ASSERT_EQUAL(HC_ERR_OFFLINE, hc_act(&HC, K_GARAGE, ACT_STOP, 0, NULL));
    TEST_ASSERT_EQUAL(HC_ERR_OFFLINE, hc_act_auto(&HC, K_GARAGE, ACT_STOP, 0, NULL, true, &id));
    TEST_ASSERT_EQUAL_UINT(calls, SEND.calls);
    TEST_ASSERT_EQUAL_UINT32(0, hc_get(&HC, K_GARAGE)->pending);

    TEST_ASSERT_EQUAL(HC_OK, hc_act_auto(&HC, K_GARAGE, ACT_CLOSE, 0, NULL, false, &id));
    TEST_ASSERT_EQUAL_UINT(calls + 1u, SEND.calls);
    TEST_ASSERT_EQUAL_INT(0, SEND.level);
    TEST_ASSERT_EQUAL_STRING(K_GARAGE, SEND.key);
    TEST_ASSERT_EQUAL_UINT32(id, SEND.request_id);
    dsrc_zw_result(&ZW, id, true, "");
    TEST_ASSERT_EQUAL_UINT32(id, R_ID);
    TEST_ASSERT_EQUAL_STRING(K_GARAGE, R_REF);

    /* AUT-015 on: the open is posted as 100; the output layer decides whether to press */
    TEST_ASSERT_EQUAL(HC_OK, hc_act_auto(&HC, K_GARAGE, ACT_OPEN, 0, NULL, true, &id));
    TEST_ASSERT_EQUAL_UINT(calls + 2u, SEND.calls);
    TEST_ASSERT_EQUAL_INT(100, SEND.level);
    TEST_ASSERT_EQUAL_STRING(K_GARAGE, SEND.key);
    TEST_ASSERT_EQUAL_UINT32(id, SEND.request_id);
    dsrc_zw_result(&ZW, id, true, "");
    TEST_ASSERT_EQUAL_UINT32(id, R_ID);
}

/* AUT-002 layer 1 against layer 6: every row the pickers offer for a Z-Wave device, with the
 * installer option on, is posted by this source through hc_act_auto (never refused as one it
 * can't do, so never "unreachable" for that); with it off the garage offers Close alone. */
static void test_every_offered_row_is_posted(void)
{
    feed_three();
    hc_register(&HC, dsrc_zw_ops(), &ZW);
    hc_refresh(&HC);
    static const struct {
        const char *ref;
        dev_kind_t kind;
    } devs[] = {{K_PLUG, DEV_SWITCH}, {K_DIMMER, DEV_LIGHT}, {K_GARAGE, DEV_COVER}};
    for (size_t k = 0; k < 3; k++) {
        hc_auto_offer_t rows[HC_AUTO_OFFER_MAX];
        const size_t n = hc_auto_offer(devs[k].kind, DSRC_ZWAVE, true, rows);
        TEST_ASSERT_GREATER_THAN_UINT(0u, n);
        for (size_t i = 0; i < n; i++) {
            const unsigned calls = SEND.calls;
            uint32_t id = 0;
            TEST_ASSERT_EQUAL(HC_OK, hc_act_auto(&HC, devs[k].ref, (dev_action_t)rows[i].action,
                                                 rows[i].value, rows[i].mode, true, &id));
            TEST_ASSERT_EQUAL_UINT(calls + 1u, SEND.calls);
            TEST_ASSERT_EQUAL_UINT32(id, SEND.request_id);
            TEST_ASSERT_EQUAL_INT(dsrc_zw_level_for(devs[k].kind, (dev_action_t)rows[i].action,
                                                    rows[i].value),
                                  SEND.level);
            dsrc_zw_result(&ZW, id, true, "");
        }
    }
    hc_auto_offer_t rows[HC_AUTO_OFFER_MAX];
    TEST_ASSERT_EQUAL_UINT(1u, hc_auto_offer(DEV_COVER, DSRC_ZWAVE, false, rows));
    TEST_ASSERT_EQUAL_UINT(ACT_CLOSE, rows[0].action);
}

/* ---- registration order ---------------------------------------------------------------------- */

static void feed_ha_full(void)
{
    dsrc_ha_link(&HA, true);
    dsrc_ha_catalog_begin(&HA);
    for (unsigned i = 0; i < DSRC_HA_MAX; i++) {
        char id[32];
        snprintf(id, sizeof(id), "light.room_%02u", i);
        dsrc_ha_catalog_add(&HA, id, "", "Room", "light", "off", NULL);
    }
}

/* HCTL-001: registered before Home Assistant, the panel's own outputs are always listed, even
 * with a full Home Assistant list (64) filling HC_MAX_DEVICES (64). The other way round they'd
 * be crowded out - which is why the platform registers Z-Wave first. */
static void test_zwave_before_home_assistant_is_never_crowded_out(void)
{
    feed_three();
    feed_ha_full();
    TEST_ASSERT_TRUE(hc_register(&HC, dsrc_zw_ops(), &ZW));
    TEST_ASSERT_TRUE(hc_register(&HC, dsrc_ha_ops(), &HA));
    hc_refresh(&HC);
    TEST_ASSERT_EQUAL_size_t(HC_MAX_DEVICES, HC.n_devices);
    TEST_ASSERT_NOT_NULL(hc_get(&HC, K_PLUG));
    TEST_ASSERT_NOT_NULL(hc_get(&HC, K_DIMMER));
    TEST_ASSERT_NOT_NULL(hc_get(&HC, K_GARAGE));
    TEST_ASSERT_EQUAL(DSRC_ZWAVE, HC.devices[0].info.source);
    TEST_ASSERT_EQUAL(DSRC_HA, HC.devices[3].info.source);

    /* the full outputs' bound, too */
    dsrc_zw_begin(&ZW);
    for (unsigned i = 0; i < DSRC_ZW_MAX; i++) {
        char key[DSRC_ZW_KEY_LEN];
        snprintf(key, sizeof(key), "zw_0a1b2c3d_%u_1_0", i + 1u);
        dsrc_zw_add(&ZW, key, DSRC_ZW_SWITCH, "", 0, true, "", 0u);
    }
    dsrc_zw_end(&ZW);
    hc_refresh(&HC);
    TEST_ASSERT_EQUAL_size_t(HC_MAX_DEVICES, HC.n_devices);
    for (unsigned i = 0; i < DSRC_ZW_MAX; i++) {
        char key[DSRC_ZW_KEY_LEN];
        snprintf(key, sizeof(key), "zw_0a1b2c3d_%u_1_0", i + 1u);
        TEST_ASSERT_NOT_NULL_MESSAGE(hc_get(&HC, key), key);
    }

    /* the contrast: Home Assistant first leaves no room */
    const hc_port_t port = {.ctx = NULL, .changed = port_changed, .result = port_result};
    hc_init(&HC, &port);
    TEST_ASSERT_TRUE(hc_register(&HC, dsrc_ha_ops(), &HA));
    TEST_ASSERT_TRUE(hc_register(&HC, dsrc_zw_ops(), &ZW));
    hc_refresh(&HC);
    TEST_ASSERT_NULL(hc_get(&HC, "zw_0a1b2c3d_1_1_0"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_list_and_kinds);
    RUN_TEST(test_states_and_link);
    RUN_TEST(test_begin_end_replaces_the_list);
    RUN_TEST(test_act_every_kind_and_action);
    RUN_TEST(test_act_offline_and_unknown);
    RUN_TEST(test_request_id_round_trip);
    RUN_TEST(test_outcome_by_origin);
    RUN_TEST(test_garage_waits_its_whole_move);
    RUN_TEST(test_automation_through_home_control);
    RUN_TEST(test_every_offered_row_is_posted);
    RUN_TEST(test_zwave_before_home_assistant_is_never_crowded_out);
    return UNITY_END();
}
