/*
 * test_dsrc_ha.c - Home Assistant as a device source on its own (dsrc_ha.h, docs/features/24):
 * what a device can do (DEV_CAP_*), since when, what a thermostat is doing, and the link drop's
 * time. What the registry would see is what dsrc_ha_ops()->list returns. The registry and the
 * other sources are tested in test_home_control.c.
 */
#include <string.h>

#include "home_control/dsrc_ha.h"
#include "unity.h"

static dsrc_ha_t HA;

static void no_changed(void *ctx, uint8_t source)
{
    (void)ctx;
    (void)source;
}

static void no_result(void *ctx, uint32_t request_id, bool ok, const char *error,
                      bool unknown)
{
    (void)ctx;
    (void)request_id;
    (void)ok;
    (void)error;
    (void)unknown;
}

void setUp(void)
{
    const dsrc_sink_t sink = {.ctx = NULL, .changed = no_changed, .result = no_result};
    dsrc_ha_init(&HA, &sink);
    dsrc_ha_link_at(&HA, true, 0u);
}

void tearDown(void)
{
}

/* What the registry would see of one device. */
static dev_info_t listed(const char *ref)
{
    static dev_info_t rows[DSRC_HA_MAX];
    const size_t n = dsrc_ha_ops()->list(&HA, rows, DSRC_HA_MAX);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(rows[i].ref, ref) == 0) {
            return rows[i];
        }
    }
    TEST_FAIL_MESSAGE(ref);
    dev_info_t none;
    memset(&none, 0, sizeof(none));
    return none;
}

static const dev_info_t *stored(const char *ref)
{
    for (size_t i = 0; i < HA.n; i++) {
        if (strcmp(HA.devices[i].ref, ref) == 0) {
            return &HA.devices[i];
        }
    }
    TEST_FAIL_MESSAGE(ref);
    return NULL;
}

/* Values as ha_link hands them over: caps 0 = the wire said nothing of them. */
static dev_values_t vals(int level, uint8_t caps)
{
    dev_values_t v;
    memset(&v, 0, sizeof(v));
    if (level >= 0) {
        v.flags = DEV_VAL_LEVEL;
        v.level = (uint8_t)level;
    }
    v.caps = caps;
    return v;
}

#define KNOWN DEV_CAP_KNOWN

/* HCTL-002, HCTL-004: when the source says what a light or a cover can do, that decides the
 * slider (DEV_VAL_LEVEL_SET) - not a level it reports, not one it reported before; when it
 * doesn't say (an older integration), today's inference stands. */
static void test_caps_decide_the_slider(void)
{
    /* a light that dims, off, no brightness: a slider all the same */
    dev_values_t v = vals(-1, KNOWN | DEV_CAP_DIM);
    dsrc_ha_catalog_add(&HA, "light.a", "A", "", "light", "off", &v);
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_LEVEL_SET, listed("light.a").values.flags);
    TEST_ASSERT_EQUAL_HEX8(KNOWN | DEV_CAP_DIM, listed("light.a").values.caps);

    /* a light that can't dim but reports a level: the level is kept, no slider */
    v = vals(40, KNOWN);
    dsrc_ha_catalog_add(&HA, "light.b", "B", "", "light", "on", &v);
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_LEVEL, listed("light.b").values.flags);
    TEST_ASSERT_EQUAL_UINT8(40, listed("light.b").values.level);

    /* remembered as dimmable from an earlier catalog (no caps then): the source's word wins */
    v = vals(30, 0u);
    dsrc_ha_catalog_add(&HA, "light.c", "C", "", "light", "on", &v);
    TEST_ASSERT_BITS_HIGH(DEV_VAL_LEVEL_SET, listed("light.c").values.flags);
    dsrc_ha_catalog_begin(&HA);
    v = vals(-1, KNOWN);
    dsrc_ha_catalog_add(&HA, "light.c", "C", "", "light", "off", &v);
    TEST_ASSERT_EQUAL_HEX8(0, listed("light.c").values.flags);
    v = vals(55, KNOWN);
    dsrc_ha_state(&HA, "light.c", "on", true, &v);
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_LEVEL, listed("light.c").values.flags);

    /* a cover that stops but has no position, reporting one: no slider; with a position: one */
    v = vals(50, KNOWN | DEV_CAP_STOP);
    dsrc_ha_catalog_add(&HA, "cover.a", "Cover A", "", "cover", "open", &v);
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_LEVEL, listed("cover.a").values.flags);
    v = vals(-1, KNOWN | DEV_CAP_POSITION);
    dsrc_ha_state(&HA, "cover.a", "open", true, &v);
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_LEVEL_SET, listed("cover.a").values.flags);

    /* caps not known: a reported level gives the slider, and a state without one keeps it */
    v = vals(60, 0u);
    dsrc_ha_catalog_add(&HA, "light.d", "D", "", "light", "on", &v);
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_LEVEL | DEV_VAL_LEVEL_SET, listed("light.d").values.flags);
    TEST_ASSERT_EQUAL_HEX8(0, listed("light.d").values.caps);
    v = vals(-1, 0u);
    dsrc_ha_state(&HA, "light.d", "off", true, &v);
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_LEVEL_SET, listed("light.d").values.flags);
    dsrc_ha_state(&HA, "light.d", "off", true, NULL);
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_LEVEL_SET, listed("light.d").values.flags);
}

/* HCTL-002: each kind keeps only its own abilities; kinds without any keep none. */
static void test_caps_kept_per_kind(void)
{
    const uint8_t all = KNOWN | DEV_CAP_STOP | DEV_CAP_POSITION | DEV_CAP_SPEED | DEV_CAP_DIM;
    static const struct {
        const char *ref;
        const char *domain;
        uint8_t in;
        uint8_t kept;
    } CASES[] = {
        {"switch.a", "switch", all, 0u},
        {"lock.a", "lock", all, 0u},
        {"climate.a", "climate", all, 0u},
        {"scene.a", "scene", all, 0u},
        {"fan.a", "fan", all, KNOWN | DEV_CAP_SPEED},
        {"fan.b", "fan", KNOWN, KNOWN},
        {"cover.a", "cover", all, KNOWN | DEV_CAP_STOP | DEV_CAP_POSITION},
        {"valve.a", "valve", all, KNOWN | DEV_CAP_STOP | DEV_CAP_POSITION},
        {"light.a", "light", KNOWN | DEV_CAP_STOP, KNOWN},
        {"light.b", "light", all, KNOWN | DEV_CAP_DIM},
        /* bits without KNOWN: the source said nothing (not a producer of ours, but safe) */
        {"cover.b", "cover", DEV_CAP_STOP, 0u},
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        dev_values_t v = vals(-1, CASES[i].in);
        dsrc_ha_catalog_add(&HA, CASES[i].ref, "", "", CASES[i].domain, "", &v);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(CASES[i].kept, listed(CASES[i].ref).values.caps,
                                       CASES[i].ref);
        dsrc_ha_state(&HA, CASES[i].ref, "", true, &v); /* a state keeps them the same way */
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(CASES[i].kept, listed(CASES[i].ref).values.caps,
                                       CASES[i].ref);
    }
}

/* HCTL-002, HCTL-006: an unavailable device whose message says nothing of what it can keeps what
 * it could (an offline tile keeps its shape); one that says gives its new word; an available
 * state replaces the values, so one without "supports" is not known again. */
static void test_caps_while_unavailable(void)
{
    dev_values_t v = vals(20, KNOWN | DEV_CAP_STOP | DEV_CAP_POSITION);
    dsrc_ha_catalog_add(&HA, "cover.a", "A", "", "cover", "open", &v);

    v = vals(-1, 0u);
    dsrc_ha_state(&HA, "cover.a", "", false, &v);
    dev_info_t d = listed("cover.a");
    TEST_ASSERT_FALSE(d.online);
    TEST_ASSERT_EQUAL_HEX8(KNOWN | DEV_CAP_STOP | DEV_CAP_POSITION, d.values.caps);
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_LEVEL_SET, d.values.flags); /* the slider, no level */
    dsrc_ha_state(&HA, "cover.a", "", false, NULL);
    TEST_ASSERT_EQUAL_HEX8(KNOWN | DEV_CAP_STOP | DEV_CAP_POSITION, listed("cover.a").values.caps);

    v = vals(-1, KNOWN | DEV_CAP_STOP);
    dsrc_ha_state(&HA, "cover.a", "", false, &v);
    TEST_ASSERT_EQUAL_HEX8(KNOWN | DEV_CAP_STOP, listed("cover.a").values.caps);
    TEST_ASSERT_EQUAL_HEX8(0, listed("cover.a").values.flags);

    v = vals(-1, 0u);
    dsrc_ha_state(&HA, "cover.a", "closed", true, &v);
    d = listed("cover.a");
    TEST_ASSERT_TRUE(d.online);
    TEST_ASSERT_EQUAL_HEX8(0, d.values.caps);
}

/* HCTL-006, HCTL-002: since is kept for every kind, available or not; what a thermostat is
 * doing only for a thermostat while it is available, and only a word of dev_hvac_t. */
static void test_since_and_hvac_action_kept(void)
{
    dsrc_ha_catalog_add(&HA, "light.a", "A", "", "light", "on", NULL);
    dev_values_t v = vals(70, 0u);
    v.since = 1791313920u;
    v.hvac_action = (uint8_t)DEV_HVAC_IDLE;
    dsrc_ha_state(&HA, "light.a", "", false, &v);
    dev_info_t d = listed("light.a");
    TEST_ASSERT_EQUAL_UINT32(1791313920u, d.values.since);
    TEST_ASSERT_EQUAL_HEX8(0, d.values.flags & DEV_VAL_LEVEL); /* not known while unavailable */
    TEST_ASSERT_EQUAL_UINT8(DEV_HVAC_UNKNOWN, d.values.hvac_action);
    v.since = 0u;
    dsrc_ha_state(&HA, "light.a", "on", true, &v);
    TEST_ASSERT_EQUAL_UINT32(0u, listed("light.a").values.since); /* a state replaces it */
    TEST_ASSERT_EQUAL_UINT8(DEV_HVAC_UNKNOWN, listed("light.a").values.hvac_action);

    v = vals(-1, 0u);
    v.since = 1791320712u;
    dsrc_ha_catalog_add(&HA, "scene.a", "Good Night", "", "scene", "", &v);
    TEST_ASSERT_EQUAL_UINT32(1791320712u, listed("scene.a").values.since);
    dsrc_ha_state(&HA, "scene.a", "", true, &v);
    TEST_ASSERT_EQUAL_UINT32(1791320712u, listed("scene.a").values.since);

    v = vals(-1, 0u);
    v.hvac_action = (uint8_t)DEV_HVAC_IDLE;
    dsrc_ha_catalog_add(&HA, "climate.a", "Hall", "", "climate", "heat", &v);
    TEST_ASSERT_EQUAL_UINT8(DEV_HVAC_IDLE, listed("climate.a").values.hvac_action);
    v.hvac_action = (uint8_t)DEV_HVAC_DEFROSTING;
    dsrc_ha_state(&HA, "climate.a", "heat", true, &v);
    TEST_ASSERT_EQUAL_UINT8(DEV_HVAC_DEFROSTING, listed("climate.a").values.hvac_action);
    v.hvac_action = (uint8_t)DEV_HVAC_COUNT; /* 9: not a word of ours */
    dsrc_ha_state(&HA, "climate.a", "heat", true, &v);
    TEST_ASSERT_EQUAL_UINT8(DEV_HVAC_UNKNOWN, listed("climate.a").values.hvac_action);
    v.hvac_action = (uint8_t)DEV_HVAC_HEATING;
    dsrc_ha_state(&HA, "climate.a", "", false, &v);
    TEST_ASSERT_EQUAL_UINT8(DEV_HVAC_UNKNOWN, listed("climate.a").values.hvac_action);
}

/* HCTL-006: when the link drops, a device that was online lists the panel's clock as since when
 * it is offline; one that was already unavailable keeps Home Assistant's time; the devices'
 * own values never hold the panel's time, so a reconnect gives the scene its run time back. */
static void test_link_drop_stamps_since(void)
{
    const uint32_t S = 1791300000u; /* B unavailable since, Home Assistant's */
    const uint32_t R = 1791320712u; /* C last ran */
    const uint32_t T = 1791330000u; /* the panel's clock at the drop */
    dsrc_ha_catalog_add(&HA, "light.a", "A", "", "light", "on", NULL);
    dsrc_ha_catalog_add(&HA, "switch.b", "B", "", "switch", "off", NULL);
    dev_values_t v = vals(-1, 0u);
    v.since = S;
    dsrc_ha_state(&HA, "switch.b", "", false, &v);
    v.since = R;
    dsrc_ha_catalog_add(&HA, "scene.c", "C", "", "scene", "", &v);

    dsrc_ha_link_at(&HA, false, T);
    TEST_ASSERT_FALSE(listed("light.a").online);
    TEST_ASSERT_EQUAL_UINT32(T, listed("light.a").values.since);
    TEST_ASSERT_EQUAL_UINT32(S, listed("switch.b").values.since);
    TEST_ASSERT_EQUAL_UINT32(T, listed("scene.c").values.since);
    TEST_ASSERT_EQUAL_UINT32(0u, stored("light.a")->values.since);
    TEST_ASSERT_EQUAL_UINT32(S, stored("switch.b")->values.since);
    TEST_ASSERT_EQUAL_UINT32(R, stored("scene.c")->values.since);
    dsrc_ha_link_at(&HA, false, T + 60u); /* already down: the first time stays */
    TEST_ASSERT_EQUAL_UINT32(T, listed("light.a").values.since);

    dsrc_ha_link_at(&HA, true, 0u);
    TEST_ASSERT_TRUE(listed("light.a").online);
    TEST_ASSERT_EQUAL_UINT32(0u, listed("light.a").values.since);
    TEST_ASSERT_EQUAL_UINT32(S, listed("switch.b").values.since);
    TEST_ASSERT_EQUAL_UINT32(R, listed("scene.c").values.since);
    /* coming up marks every device online, as before; the integration's states follow at once
     * and say B is still unavailable */
    v.since = S;
    dsrc_ha_state(&HA, "switch.b", "", false, &v);
    TEST_ASSERT_FALSE(listed("switch.b").online);

    /* the panel's clock not set: no time */
    dsrc_ha_link_at(&HA, false, 0u);
    TEST_ASSERT_EQUAL_UINT32(0u, listed("light.a").values.since);
    TEST_ASSERT_EQUAL_UINT32(S, listed("switch.b").values.since);
    TEST_ASSERT_EQUAL_UINT32(0u, listed("scene.c").values.since);
    dev_info_t at0[DSRC_HA_MAX];
    const size_t n0 = dsrc_ha_ops()->list(&HA, at0, DSRC_HA_MAX);

    /* dsrc_ha_link is dsrc_ha_link_at with no time */
    dsrc_ha_link_at(&HA, true, 0u);
    dsrc_ha_state(&HA, "switch.b", "", false, &v); /* still unavailable, as before */
    dsrc_ha_link(&HA, false);
    dev_info_t plain[DSRC_HA_MAX];
    const size_t n1 = dsrc_ha_ops()->list(&HA, plain, DSRC_HA_MAX);
    TEST_ASSERT_EQUAL_size_t(n0, n1);
    for (size_t i = 0; i < n0; i++) {
        TEST_ASSERT_EQUAL_STRING(at0[i].ref, plain[i].ref);
        TEST_ASSERT_EQUAL(at0[i].online, plain[i].online);
        TEST_ASSERT_EQUAL_UINT32(at0[i].values.since, plain[i].values.since);
    }

    /* a new catalog while down lists no drop time: the rows are the catalog's */
    dsrc_ha_link_at(&HA, true, 0u);
    dsrc_ha_link_at(&HA, false, T);
    dsrc_ha_catalog_begin(&HA);
    dsrc_ha_catalog_add(&HA, "light.a", "A", "", "light", "on", NULL);
    TEST_ASSERT_EQUAL_UINT32(0u, listed("light.a").values.since);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_caps_decide_the_slider);
    RUN_TEST(test_caps_kept_per_kind);
    RUN_TEST(test_caps_while_unavailable);
    RUN_TEST(test_since_and_hvac_action_kept);
    RUN_TEST(test_link_drop_stamps_since);
    return UNITY_END();
}
