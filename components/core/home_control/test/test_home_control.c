/*
 * test_home_control.c - the Devices model (components/core/home_control, docs/features/24): the registry
 * and its merge of the sources, the security classes (a user code before an unlock, an opening,
 * or a cover position that isn't fully closed), one action at a time per device with a bounded
 * lifetime and its result, and Home Assistant as a device source (the domain and action tables
 * of docs/features/23 protocol.md, "the Devices screen").
 * Requirements: HCTL-001, HCTL-002, HCTL-004, HCTL-005, HCTL-006, HCTL-011, HCTL-012, AUT-002, AUT-015
 * No alarm engine is involved: home_control never touches alarm_core.
 */
#include <stdio.h>
#include <string.h>

#include "home_control/dsrc_ha.h"
#include "home_control/dsrc_zw.h"
#include "home_control/home_control.h"
#include "unity.h"

/* ---- harness: a recording port, a fake source, a fake Home Assistant sender --------------- */

#define FAKE_MAX 8

typedef struct {
    bool online;
    dev_info_t devs[FAKE_MAX];
    size_t n;
    bool act_returns; /* what act() answers right away */
    unsigned act_calls;
    char act_ref[DEV_REF_LEN];
    int act_action;
    int32_t act_value;
    char act_mode[DEV_MODE_LEN];
    uint32_t act_id;
} fake_t;

typedef struct {
    unsigned calls;
    char entity[DEV_REF_LEN];
    char action[32];
    bool has_value;
    int32_t value;
    char mode[DEV_MODE_LEN];
    uint32_t request_id;
    bool returns;
} sender_t;

static hc_t HC;
static dsrc_ha_t HA;
static fake_t FAKE;
static sender_t SEND;

static unsigned CHANGES;
static unsigned RESULTS;
static char R_REF[DEV_REF_LEN];
static bool R_OK;
static char R_ERR[HC_ERROR_LEN];
static uint32_t R_ID;

static void port_changed(void *ctx)
{
    (void)ctx;
    CHANGES++;
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

static bool fake_online(void *ctx)
{
    return ((const fake_t *)ctx)->online;
}

static size_t fake_list(void *ctx, dev_info_t *out, size_t max)
{
    const fake_t *f = ctx;
    size_t n = 0;
    for (size_t i = 0; i < f->n && n < max; i++) {
        out[n++] = f->devs[i];
    }
    return n;
}

static bool fake_act(void *ctx, const char *ref, dev_action_t action, int32_t value,
                     const char *mode, uint32_t request_id)
{
    fake_t *f = ctx;
    f->act_calls++;
    snprintf(f->act_ref, sizeof(f->act_ref), "%s", ref);
    f->act_action = (int)action;
    f->act_value = value;
    snprintf(f->act_mode, sizeof(f->act_mode), "%s", mode != NULL ? mode : "");
    f->act_id = request_id;
    return f->act_returns;
}

/* A second, native source (the Zigbee slot of docs/features/20) so the tests can have two kinds. */
static const device_source_ops_t FAKE_OPS = {
    .kind = DSRC_ZIGBEE,
    .name = "fake",
    .online = fake_online,
    .list = fake_list,
    .act = fake_act,
};

static bool sender(void *ctx, const char *entity, const char *action, bool has_value,
                   int32_t value, const char *mode, uint32_t request_id)
{
    sender_t *s = ctx;
    s->calls++;
    snprintf(s->entity, sizeof(s->entity), "%s", entity);
    snprintf(s->action, sizeof(s->action), "%s", action);
    s->has_value = has_value;
    s->value = value;
    snprintf(s->mode, sizeof(s->mode), "%s", mode != NULL ? mode : "");
    s->request_id = request_id;
    return s->returns;
}

static void fake_add(fake_t *f, const char *ref, dev_kind_t kind, const char *name,
                     const char *area, const char *state, bool online)
{
    dev_info_t *d = &f->devs[f->n++];
    memset(d, 0, sizeof(*d));
    d->source = DSRC_ZIGBEE;
    d->kind = (uint8_t)kind;
    snprintf(d->ref, sizeof(d->ref), "%s", ref);
    snprintf(d->name, sizeof(d->name), "%s", name);
    snprintf(d->area, sizeof(d->area), "%s", area);
    snprintf(d->state, sizeof(d->state), "%s", state);
    d->online = online;
}

void setUp(void)
{
    const hc_port_t port = {.ctx = NULL, .changed = port_changed, .result = port_result};
    memset(&FAKE, 0, sizeof(FAKE));
    memset(&SEND, 0, sizeof(SEND));
    FAKE.online = true;
    FAKE.act_returns = true;
    SEND.returns = true;
    CHANGES = 0;
    RESULTS = 0;
    R_REF[0] = '\0';
    R_ERR[0] = '\0';
    R_OK = false;
    R_ID = 0;
    hc_init(&HC, &port);
    dsrc_ha_init(&HA, hc_sink(&HC));
    dsrc_ha_set_sender(&HA, sender, &SEND);
}

void tearDown(void) {}

/* Registers the fake source with two devices and refreshes: a porch light and a front lock. */
static void with_two_fake_devices(void)
{
    fake_add(&FAKE, "zb/porch", DEV_LIGHT, "Porch", "Outside", "on", true);
    fake_add(&FAKE, "zb/front", DEV_LOCK, "Front Door", "Hall", "locked", true);
    TEST_ASSERT_TRUE(hc_register(&HC, &FAKE_OPS, &FAKE));
    hc_refresh(&HC);
}

/* ---- the registry --------------------------------------------------------------------------- */

/* HCTL-001: a source registers, its devices come through the registry, nothing else. */
static void test_register_list_and_get(void)
{
    with_two_fake_devices();

    const hc_device_t *out[HC_MAX_DEVICES];
    TEST_ASSERT_EQUAL_size_t(2, hc_list(&HC, out, HC_MAX_DEVICES));
    TEST_ASSERT_EQUAL_STRING("zb/porch", out[0]->info.ref);
    TEST_ASSERT_EQUAL_STRING("Porch", out[0]->info.name);
    TEST_ASSERT_EQUAL_STRING("Outside", out[0]->info.area);
    TEST_ASSERT_EQUAL_STRING("on", out[0]->info.state);
    TEST_ASSERT_EQUAL_UINT8(DEV_LIGHT, out[0]->info.kind);
    TEST_ASSERT_EQUAL_UINT8(DSRC_ZIGBEE, out[0]->info.source);
    TEST_ASSERT_TRUE(out[0]->info.online);
    TEST_ASSERT_EQUAL_UINT32(0, out[0]->pending);
    TEST_ASSERT_FALSE(out[0]->failed);

    const hc_device_t *lock = hc_get(&HC, "zb/front");
    TEST_ASSERT_NOT_NULL(lock);
    TEST_ASSERT_EQUAL_UINT8(DEV_LOCK, lock->info.kind);
    TEST_ASSERT_NULL(hc_get(&HC, "zb/nothing"));
    TEST_ASSERT_NULL(hc_get(&HC, ""));

    /* hc_list never writes past max */
    const hc_device_t *one[1] = {NULL};
    TEST_ASSERT_EQUAL_size_t(1, hc_list(&HC, one, 1));
    TEST_ASSERT_EQUAL_STRING("zb/porch", one[0]->info.ref);
}

/* HCTL-001: one source per kind, and never more than HC_MAX_SOURCES of them. */
static void test_one_source_per_kind(void)
{
    static fake_t other;
    memset(&other, 0, sizeof(other));

    TEST_ASSERT_TRUE(hc_register(&HC, &FAKE_OPS, &FAKE));
    TEST_ASSERT_FALSE(hc_register(&HC, &FAKE_OPS, &other)); /* same kind: refused */
    TEST_ASSERT_EQUAL_size_t(1, HC.n_sources);

    TEST_ASSERT_TRUE(hc_register(&HC, dsrc_ha_ops(), &HA)); /* a different kind fits */
    TEST_ASSERT_FALSE(hc_register(&HC, dsrc_ha_ops(), &HA)); /* same kind again: refused */
    static dsrc_zw_t zw;
    dsrc_zw_init(&zw, hc_sink(&HC));
    TEST_ASSERT_TRUE(hc_register(&HC, dsrc_zw_ops(), &zw)); /* the third kind (BL-169) */
    TEST_ASSERT_EQUAL_size_t(HC_MAX_SOURCES, HC.n_sources);
    TEST_ASSERT_FALSE(hc_register(&HC, &FAKE_OPS, &other)); /* full */
    TEST_ASSERT_FALSE(hc_register(&HC, NULL, &FAKE));
}

/* HCTL-002: a refresh re-reads every source; a device that is still there keeps what the
 * registry knows about it (a pending action, a failure), one that went away drops. */
static void test_refresh_merges_and_drops(void)
{
    with_two_fake_devices();
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/front", ACT_LOCK, 0, NULL));
    const uint32_t id = hc_get(&HC, "zb/front")->pending;
    TEST_ASSERT_NOT_EQUAL(0, id);

    /* the porch light reports a new state, the lock is unchanged, a fan appears */
    snprintf(FAKE.devs[0].state, sizeof(FAKE.devs[0].state), "off");
    fake_add(&FAKE, "zb/fan", DEV_FAN, "Study Fan", "Study", "off", true);
    hc_refresh(&HC);

    TEST_ASSERT_EQUAL_size_t(3, HC.n_devices);
    TEST_ASSERT_EQUAL_STRING("off", hc_get(&HC, "zb/porch")->info.state);
    TEST_ASSERT_EQUAL_UINT32(id, hc_get(&HC, "zb/front")->pending); /* carried over */
    TEST_ASSERT_NOT_NULL(hc_get(&HC, "zb/fan"));

    /* the lock goes away while its action is still under way: the row drops with it, and
     * HCTL-012: its pending action fails as "offline" with its id - a caller waiting on that
     * id always hears back */
    FAKE.n = 1;
    hc_refresh(&HC);
    TEST_ASSERT_EQUAL_size_t(1, HC.n_devices);
    TEST_ASSERT_NULL(hc_get(&HC, "zb/front"));
    TEST_ASSERT_EQUAL_UINT(1, RESULTS);
    TEST_ASSERT_EQUAL_UINT32(id, R_ID);
    TEST_ASSERT_EQUAL_STRING("zb/front", R_REF);
    TEST_ASSERT_FALSE(R_OK);
    TEST_ASSERT_EQUAL_STRING("offline", R_ERR);

    /* the late answer finds nobody and tells the UI nothing more */
    hc_sink(&HC)->result(&HC, id, true, "", false);
    TEST_ASSERT_EQUAL_UINT(1, RESULTS);

    /* a dropped row with nothing pending says nothing at all */
    FAKE.n = 0;
    hc_refresh(&HC);
    TEST_ASSERT_EQUAL_size_t(0, HC.n_devices);
    TEST_ASSERT_EQUAL_UINT(1, RESULTS);
}

/* HCTL-002: a device with no ref, or of no kind, is not something the screen can show. */
static void test_refresh_skips_unusable_rows(void)
{
    fake_add(&FAKE, "", DEV_LIGHT, "nameless", "", "on", true);
    fake_add(&FAKE, "zb/ok", DEV_SWITCH, "Plug", "Study", "on", true);
    fake_add(&FAKE, "zb/odd", DEV_NONE, "Odd", "", "", true);
    TEST_ASSERT_TRUE(hc_register(&HC, &FAKE_OPS, &FAKE));
    hc_refresh(&HC);

    TEST_ASSERT_EQUAL_size_t(1, HC.n_devices);
    TEST_ASSERT_EQUAL_STRING("zb/ok", HC.devices[0].info.ref);
}

/* HCTL-001: a source reports through the registry's sink, which carries the registry itself. */
static void test_sink_changed_reaches_the_port(void)
{
    const dsrc_sink_t *sink = hc_sink(&HC);
    TEST_ASSERT_EQUAL_PTR(&HC, sink->ctx);
    sink->changed(sink->ctx, DSRC_ZIGBEE);
    TEST_ASSERT_EQUAL_UINT(1, CHANGES);
}

/* ---- what an action means --------------------------------------------------------------------- */

/* HCTL-005: unlocking, opening a cover or a garage door, opening a water valve ask for a user
 * code; locking and closing never do, and nothing else does. value never matters except for a
 * cover's ACT_SET_LEVEL (R15, HCTL-012's neighbour: see test_needs_code_cover_set_level). */
static void test_needs_code(void)
{
    for (int k = DEV_NONE; k < DEV_KIND_COUNT; k++) {
        for (int a = ACT_NONE; a < ACT_COUNT; a++) {
            const dev_kind_t kind = (dev_kind_t)k;
            const dev_action_t action = (dev_action_t)a;
            if (kind == DEV_COVER && action == ACT_SET_LEVEL) {
                continue; /* value-dependent: its own test */
            }
            const bool expect = action == ACT_UNLOCK ||
                                (action == ACT_OPEN && (kind == DEV_COVER || kind == DEV_VALVE));
            TEST_ASSERT_EQUAL_MESSAGE(expect, hc_needs_code(kind, action, 0),
                                      "hc_needs_code disagrees with docs/features/24");
            /* value is irrelevant for every pair but the cover's set-level */
            TEST_ASSERT_EQUAL(expect, hc_needs_code(kind, action, 100));
        }
    }
    TEST_ASSERT_TRUE(hc_needs_code(DEV_LOCK, ACT_UNLOCK, 0));
    TEST_ASSERT_FALSE(hc_needs_code(DEV_LOCK, ACT_LOCK, 0));
    TEST_ASSERT_TRUE(hc_needs_code(DEV_COVER, ACT_OPEN, 0));
    TEST_ASSERT_FALSE(hc_needs_code(DEV_COVER, ACT_CLOSE, 0));
    TEST_ASSERT_TRUE(hc_needs_code(DEV_VALVE, ACT_OPEN, 0));
    TEST_ASSERT_FALSE(hc_needs_code(DEV_LIGHT, ACT_ON, 0));
}

/* ---- what an automation may do (HCTL-011 layer 5, AUT-002, AUT-015) ------------------------ */

/* The less secure pairs, written out (docs/features/41 DESIGN section 3, D25). */
static bool less_secure_expected(dev_kind_t kind, dev_action_t action, int32_t value)
{
    if (action == ACT_UNLOCK) {
        return true;
    }
    if (action == ACT_OPEN) {
        return kind == DEV_COVER || kind == DEV_VALVE;
    }
    if (kind == DEV_COVER && action == ACT_SET_LEVEL) {
        return value != 0;
    }
    return kind == DEV_COVER && action == ACT_STOP;
}

/* The one table docs/features/41 DESIGN section 3 allows, as (kind, action, value, mode,
 * less_secure). */
static bool auto_expected(dev_kind_t kind, dev_action_t action, int32_t value, const char *mode,
                          bool less_secure)
{
    if (action != ACT_SET_MODE && mode != NULL && mode[0] != '\0') {
        return false; /* a stray mode word would travel with the action (ha_link sends it) */
    }
    if (!less_secure && less_secure_expected(kind, action, value)) {
        return false;
    }
    switch (kind) {
    case DEV_LIGHT:
        return (action == ACT_ON && value == -1) || action == ACT_OFF || action == ACT_TOGGLE;
    case DEV_FAN:
        return (action == ACT_ON && value == -1) || action == ACT_OFF;
    case DEV_SWITCH:
        return action == ACT_ON || action == ACT_OFF || action == ACT_TOGGLE;
    case DEV_LOCK:
        return action == ACT_LOCK || action == ACT_UNLOCK;
    case DEV_COVER:
        return action == ACT_CLOSE || action == ACT_OPEN || action == ACT_STOP;
    case DEV_VALVE:
        return action == ACT_CLOSE || action == ACT_OPEN;
    case DEV_CLIMATE:
        return action == ACT_SET_MODE && mode != NULL && strcmp(mode, "off") == 0;
    case DEV_SCENE:
        return action == ACT_RUN;
    default:
        return false;
    }
}

/* AUT-015 / D25: hc_auto_less_secure names exactly the pairs that need a code, and a cover's
 * stop. */
static void test_auto_less_secure_list(void)
{
    static const int32_t values[] = {-1, 0, 1, 50, 100};
    for (int k = DEV_NONE; k <= DEV_KIND_COUNT; k++) {
        for (int a = ACT_NONE; a <= ACT_COUNT; a++) {
            for (size_t v = 0; v < sizeof(values) / sizeof(values[0]); v++) {
                const dev_kind_t kind = (dev_kind_t)k;
                const dev_action_t action = (dev_action_t)a;
                const bool got = hc_auto_less_secure(kind, action, values[v]);
                TEST_ASSERT_EQUAL_MESSAGE(less_secure_expected(kind, action, values[v]), got,
                                          "hc_auto_less_secure disagrees with 41 DESIGN 3");
                TEST_ASSERT_EQUAL(hc_needs_code(kind, action, values[v]) ||
                                      (kind == DEV_COVER && action == ACT_STOP),
                                  got);
            }
        }
    }
    TEST_ASSERT_TRUE(hc_auto_less_secure(DEV_LOCK, ACT_UNLOCK, 0));
    TEST_ASSERT_TRUE(hc_auto_less_secure(DEV_COVER, ACT_OPEN, 0));
    TEST_ASSERT_TRUE(hc_auto_less_secure(DEV_COVER, ACT_STOP, 0));
    TEST_ASSERT_TRUE(hc_auto_less_secure(DEV_VALVE, ACT_OPEN, 0));
    TEST_ASSERT_FALSE(hc_auto_less_secure(DEV_LOCK, ACT_LOCK, 0));
    TEST_ASSERT_FALSE(hc_auto_less_secure(DEV_COVER, ACT_CLOSE, 0));
    TEST_ASSERT_FALSE(hc_auto_less_secure(DEV_VALVE, ACT_CLOSE, 0));
    TEST_ASSERT_FALSE(hc_auto_less_secure(DEV_SWITCH, ACT_TOGGLE, 0));
    TEST_ASSERT_FALSE(hc_auto_less_secure(DEV_LIGHT, ACT_TOGGLE, 0));
}

/* HCTL-011 / AUT-002 / AUT-015: every kind x action pair, with the values and modes that
 * matter, with the installer option on and off, against the table; nothing allowed falls
 * outside hc_action_valid, and with the option off nothing allowed is less secure. */
/* AUT-002 layer 1, AUT-015: what the pickers offer, as a table - every kind x {Home Assistant,
 * Z-Wave} x {the installer option on, off}. Rows are written "action" or "action:mode", in the
 * order on, off, toggle, lock, unlock, close, open, stop, set-mode, run. */
static const char *offer_word(uint8_t action)
{
    static const char *const w[ACT_COUNT] = {"none",   "on",    "off",  "toggle", "lock",
                                             "unlock", "open",  "close", "stop",  "level",
                                             "temp",   "mode",  "run"};
    return action < ACT_COUNT ? w[action] : "?";
}

static void offer_text(dev_kind_t kind, uint8_t source, bool less_secure, char *buf, size_t cap)
{
    hc_auto_offer_t rows[HC_AUTO_OFFER_MAX];
    memset(rows, 0x5A, sizeof(rows));
    const size_t n = hc_auto_offer(kind, source, less_secure, rows);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(HC_AUTO_OFFER_MAX, n);
    size_t at = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        const hc_auto_offer_t *o = &rows[i];
        /* each row is one the core accepts with the same flag, and the source can carry out */
        TEST_ASSERT_EQUAL_INT32(-1, o->value);
        TEST_ASSERT_TRUE(memchr(o->mode, '\0', sizeof(o->mode)) != NULL);
        TEST_ASSERT_TRUE_MESSAGE(hc_auto_allowed(kind, (dev_action_t)o->action, o->value,
                                                 o->mode, less_secure),
                                 "an offered row hc_auto_allowed refuses");
        if (source == DSRC_HA) {
            TEST_ASSERT_NOT_NULL(dsrc_ha_action_word(kind, (dev_action_t)o->action));
        } else {
            TEST_ASSERT_TRUE(dsrc_zw_level_for(kind, (dev_action_t)o->action, o->value) >= 0);
        }
        at += (size_t)snprintf(buf + at, cap - at, "%s%s%s%s", i ? " " : "",
                               offer_word(o->action), o->mode[0] ? ":" : "", o->mode);
    }
}

static void test_auto_offer_table(void)
{
    static const struct {
        dev_kind_t kind;
        uint8_t source;
        const char *on;  /* the installer option on */
        const char *off; /* and off */
    } k_table[] = {
        {DEV_NONE, DSRC_HA, "", ""},
        {DEV_LIGHT, DSRC_HA, "on off toggle", "on off toggle"},
        {DEV_SWITCH, DSRC_HA, "on off toggle", "on off toggle"},
        {DEV_LOCK, DSRC_HA, "lock unlock", "lock"},
        {DEV_CLIMATE, DSRC_HA, "mode:off", "mode:off"},
        {DEV_COVER, DSRC_HA, "close open stop", "close"},
        {DEV_FAN, DSRC_HA, "on off", "on off"},
        {DEV_VALVE, DSRC_HA, "close open", "close"},
        {DEV_SCENE, DSRC_HA, "run", "run"},
        {DEV_SCRIPT, DSRC_HA, "", ""},
        {DEV_KIND_COUNT, DSRC_HA, "", ""},
        /* Z-Wave: no toggle on anything, no stop for a garage door (dsrc_zw_level_for), and only
         * switches, dimmers and garage doors exist there */
        {DEV_NONE, DSRC_ZWAVE, "", ""},
        {DEV_LIGHT, DSRC_ZWAVE, "on off", "on off"},
        {DEV_SWITCH, DSRC_ZWAVE, "on off", "on off"},
        {DEV_LOCK, DSRC_ZWAVE, "", ""},
        {DEV_CLIMATE, DSRC_ZWAVE, "", ""},
        {DEV_COVER, DSRC_ZWAVE, "close open", "close"},
        {DEV_FAN, DSRC_ZWAVE, "", ""},
        {DEV_VALVE, DSRC_ZWAVE, "", ""},
        {DEV_SCENE, DSRC_ZWAVE, "", ""},
        {DEV_SCRIPT, DSRC_ZWAVE, "", ""},
        {DEV_KIND_COUNT, DSRC_ZWAVE, "", ""},
    };
    char got[96];
    for (size_t i = 0; i < sizeof k_table / sizeof k_table[0]; i++) {
        char msg[64];
        snprintf(msg, sizeof msg, "kind %d source %u", (int)k_table[i].kind,
                 (unsigned)k_table[i].source);
        offer_text(k_table[i].kind, k_table[i].source, true, got, sizeof got);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(k_table[i].on, got, msg);
        offer_text(k_table[i].kind, k_table[i].source, false, got, sizeof got);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(k_table[i].off, got, msg);
    }
    /* every kind is in the table for both sources */
    TEST_ASSERT_EQUAL_UINT(2u * ((unsigned)DEV_KIND_COUNT + 1u),
                           sizeof k_table / sizeof k_table[0]);
    /* a source this firmware can't act on (Zigbee, none) offers nothing */
    for (int k = DEV_NONE; k < DEV_KIND_COUNT; k++) {
        hc_auto_offer_t rows[HC_AUTO_OFFER_MAX];
        TEST_ASSERT_EQUAL_UINT(0u, hc_auto_offer((dev_kind_t)k, DSRC_ZIGBEE, true, rows));
        TEST_ASSERT_EQUAL_UINT(0u, hc_auto_offer((dev_kind_t)k, DSRC_NONE, true, rows));
    }
    TEST_ASSERT_EQUAL_UINT(0u, hc_auto_offer(DEV_LIGHT, DSRC_HA, true, NULL));
}

static void test_auto_allowed_table(void)
{
    static const int32_t values[] = {-1, 0, 1, 50, 100};
    static const char *const modes[] = {NULL, "", "off", "heat", "cool", "auto", "Off", "off "};
    for (int ls = 0; ls <= 1; ls++) {
        for (int k = DEV_NONE; k <= DEV_KIND_COUNT; k++) {
            for (int a = ACT_NONE; a <= ACT_COUNT; a++) {
                for (size_t v = 0; v < sizeof(values) / sizeof(values[0]); v++) {
                    for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
                        const dev_kind_t kind = (dev_kind_t)k;
                        const dev_action_t action = (dev_action_t)a;
                        const bool got =
                            hc_auto_allowed(kind, action, values[v], modes[m], ls != 0);
                        TEST_ASSERT_EQUAL_MESSAGE(
                            auto_expected(kind, action, values[v], modes[m], ls != 0), got,
                            "hc_auto_allowed disagrees with 41 DESIGN 3");
                        if (got) {
                            TEST_ASSERT_TRUE(hc_action_valid(kind, action));
                            if (ls == 0) {
                                TEST_ASSERT_FALSE(hc_auto_less_secure(kind, action, values[v]));
                            }
                        }
                    }
                }
            }
        }
    }
    /* allowed either way */
    for (int ls = 0; ls <= 1; ls++) {
        const bool o = ls != 0;
        TEST_ASSERT_TRUE(hc_auto_allowed(DEV_LIGHT, ACT_ON, -1, NULL, o));
        TEST_ASSERT_TRUE(hc_auto_allowed(DEV_LIGHT, ACT_OFF, 0, NULL, o));
        TEST_ASSERT_TRUE(hc_auto_allowed(DEV_LIGHT, ACT_TOGGLE, 0, NULL, o));
        TEST_ASSERT_TRUE(hc_auto_allowed(DEV_FAN, ACT_ON, -1, NULL, o));
        TEST_ASSERT_TRUE(hc_auto_allowed(DEV_FAN, ACT_OFF, -1, NULL, o));
        TEST_ASSERT_TRUE(hc_auto_allowed(DEV_SWITCH, ACT_ON, -1, NULL, o));
        TEST_ASSERT_TRUE(hc_auto_allowed(DEV_SWITCH, ACT_OFF, -1, NULL, o));
        TEST_ASSERT_TRUE(hc_auto_allowed(DEV_SWITCH, ACT_TOGGLE, 0, NULL, o));
        TEST_ASSERT_TRUE(hc_auto_allowed(DEV_LOCK, ACT_LOCK, 0, NULL, o));
        TEST_ASSERT_TRUE(hc_auto_allowed(DEV_COVER, ACT_CLOSE, 0, NULL, o));
        TEST_ASSERT_TRUE(hc_auto_allowed(DEV_VALVE, ACT_CLOSE, 0, NULL, o));
        TEST_ASSERT_TRUE(hc_auto_allowed(DEV_CLIMATE, ACT_SET_MODE, 0, "off", o));
        TEST_ASSERT_TRUE(hc_auto_allowed(DEV_SCENE, ACT_RUN, 0, NULL, o));
        /* never: a light's or fan's value 0 is brightness / speed 0, not "on" */
        TEST_ASSERT_FALSE(hc_auto_allowed(DEV_LIGHT, ACT_ON, 0, NULL, o));
        TEST_ASSERT_FALSE(hc_auto_allowed(DEV_LIGHT, ACT_ON, 50, NULL, o));
        TEST_ASSERT_FALSE(hc_auto_allowed(DEV_FAN, ACT_ON, 0, NULL, o));
        /* never: positions, levels, temperatures */
        TEST_ASSERT_FALSE(hc_auto_allowed(DEV_COVER, ACT_SET_LEVEL, 0, NULL, o)); /* saved as CLOSE */
        TEST_ASSERT_FALSE(hc_auto_allowed(DEV_COVER, ACT_SET_LEVEL, 100, NULL, o));
        TEST_ASSERT_FALSE(hc_auto_allowed(DEV_LIGHT, ACT_SET_LEVEL, 0, NULL, o));
        TEST_ASSERT_FALSE(hc_auto_allowed(DEV_FAN, ACT_SET_LEVEL, 0, NULL, o));
        TEST_ASSERT_FALSE(hc_auto_allowed(DEV_CLIMATE, ACT_SET_TEMP, 2150, NULL, o));
        /* a climate mode other than exactly "off" */
        TEST_ASSERT_FALSE(hc_auto_allowed(DEV_CLIMATE, ACT_SET_MODE, 0, "heat", o));
        TEST_ASSERT_FALSE(hc_auto_allowed(DEV_CLIMATE, ACT_SET_MODE, 0, "OFF", o));
        TEST_ASSERT_FALSE(hc_auto_allowed(DEV_CLIMATE, ACT_SET_MODE, 0, "offx", o));
        TEST_ASSERT_FALSE(hc_auto_allowed(DEV_CLIMATE, ACT_SET_MODE, 0, "", o));
        TEST_ASSERT_FALSE(hc_auto_allowed(DEV_CLIMATE, ACT_SET_MODE, 0, NULL, o));
        /* a mode word only with ACT_SET_MODE: an empty one is no mode */
        TEST_ASSERT_FALSE(hc_auto_allowed(DEV_LOCK, ACT_LOCK, 0, "off", o));
        TEST_ASSERT_FALSE(hc_auto_allowed(DEV_COVER, ACT_CLOSE, 0, "open", o));
        TEST_ASSERT_FALSE(hc_auto_allowed(DEV_LOCK, ACT_UNLOCK, 0, "off", o));
        TEST_ASSERT_TRUE(hc_auto_allowed(DEV_LOCK, ACT_LOCK, 0, "", o));
        /* a script never; a kind no table has never */
        TEST_ASSERT_FALSE(hc_auto_allowed(DEV_SCRIPT, ACT_RUN, 0, NULL, o));
        TEST_ASSERT_FALSE(hc_auto_allowed(DEV_NONE, ACT_ON, -1, NULL, o));
        TEST_ASSERT_FALSE(hc_auto_allowed(DEV_KIND_COUNT, ACT_OFF, -1, NULL, o));
        TEST_ASSERT_FALSE(hc_auto_allowed((dev_kind_t)200, ACT_OFF, -1, NULL, o));
        TEST_ASSERT_FALSE(hc_auto_allowed(DEV_LIGHT, (dev_action_t)200, -1, NULL, o));
    }
    /* the less secure pairs: only with the installer option (D25) */
    TEST_ASSERT_FALSE(hc_auto_allowed(DEV_LOCK, ACT_UNLOCK, 0, NULL, false));
    TEST_ASSERT_FALSE(hc_auto_allowed(DEV_COVER, ACT_OPEN, 0, NULL, false));
    TEST_ASSERT_FALSE(hc_auto_allowed(DEV_COVER, ACT_STOP, 0, NULL, false));
    TEST_ASSERT_FALSE(hc_auto_allowed(DEV_VALVE, ACT_OPEN, 0, NULL, false));
    TEST_ASSERT_TRUE(hc_auto_allowed(DEV_LOCK, ACT_UNLOCK, 0, NULL, true));
    TEST_ASSERT_TRUE(hc_auto_allowed(DEV_COVER, ACT_OPEN, 0, NULL, true));
    TEST_ASSERT_TRUE(hc_auto_allowed(DEV_COVER, ACT_STOP, 0, NULL, true));
    TEST_ASSERT_TRUE(hc_auto_allowed(DEV_VALVE, ACT_OPEN, 0, NULL, true));
}

/* AUT-015: hc_act_auto refuses an unlock with the installer option off - nothing sent, nothing
 * pending - and sends it with the option on. */
static void test_act_auto_less_secure(void)
{
    with_two_fake_devices();
    uint32_t id = 99u;
    TEST_ASSERT_EQUAL(HC_ERR_NOT_ALLOWED,
                      hc_act_auto(&HC, "zb/front", ACT_UNLOCK, 0, NULL, false, &id));
    TEST_ASSERT_EQUAL_UINT32(0, id);
    TEST_ASSERT_EQUAL_UINT(0, FAKE.act_calls);
    TEST_ASSERT_EQUAL_UINT32(0, hc_get(&HC, "zb/front")->pending);

    TEST_ASSERT_EQUAL(HC_OK, hc_act_auto(&HC, "zb/front", ACT_UNLOCK, 0, NULL, true, &id));
    TEST_ASSERT_EQUAL_UINT(1, FAKE.act_calls);
    TEST_ASSERT_NOT_EQUAL(0, id);
    TEST_ASSERT_EQUAL_UINT32(id, FAKE.act_id);
    /* a lock's Lock needs no option */
    hc_sink(&HC)->result(&HC, id, true, "", false);
    TEST_ASSERT_EQUAL(HC_OK, hc_act_auto(&HC, "zb/front", ACT_LOCK, 0, NULL, false, &id));
    TEST_ASSERT_EQUAL_UINT(2, FAKE.act_calls);
}

/* HCTL-011 / AUT-002: hc_act_auto judges the device's live kind - a ref an automation saved as
 * a light that now names a lock refuses ACT_ON as not allowed, before anything is sent - and
 * keeps hc_act's other refusals and their order. HCTL-012: the id it returns is the one the
 * source got and the one the result carries. */
static void test_act_auto(void)
{
    with_two_fake_devices();
    uint32_t id = 99u;

    /* allowed: sent, and the id comes back three ways */
    TEST_ASSERT_EQUAL(HC_OK, hc_act_auto(&HC, "zb/porch", ACT_ON, -1, NULL, false, &id));
    TEST_ASSERT_EQUAL_UINT(1, FAKE.act_calls);
    TEST_ASSERT_NOT_EQUAL(0, id);
    TEST_ASSERT_EQUAL_UINT32(id, FAKE.act_id);
    TEST_ASSERT_EQUAL_UINT32(id, hc_get(&HC, "zb/porch")->pending);
    hc_sink(&HC)->result(&HC, id, true, "", false);
    TEST_ASSERT_EQUAL_UINT(1, RESULTS);
    TEST_ASSERT_EQUAL_UINT32(id, R_ID);
    TEST_ASSERT_TRUE(R_OK);

    /* a failed answer carries the id too */
    TEST_ASSERT_EQUAL(HC_OK, hc_act_auto(&HC, "zb/front", ACT_LOCK, 0, NULL, false, &id));
    hc_sink(&HC)->result(&HC, id, false, "failed", false);
    TEST_ASSERT_EQUAL_UINT32(id, R_ID);
    TEST_ASSERT_FALSE(R_OK);

    /* so does a timeout */
    TEST_ASSERT_EQUAL(HC_OK, hc_act_auto(&HC, "zb/porch", ACT_OFF, -1, NULL, false, &id));
    hc_tick(&HC, 1);
    hc_tick(&HC, 1u + HC_ACT_TIMEOUT_MS);
    TEST_ASSERT_EQUAL_UINT32(id, R_ID);
    TEST_ASSERT_EQUAL_STRING("timeout", R_ERR);
    const unsigned sent = FAKE.act_calls;

    /* the live kind: zb/porch is a lock now; ACT_ON doesn't even fit it (hc_act's order) */
    FAKE.devs[0].kind = (uint8_t)DEV_LOCK;
    hc_refresh(&HC);
    id = 99u;
    TEST_ASSERT_EQUAL(HC_ERR_ARG, hc_act_auto(&HC, "zb/porch", ACT_ON, -1, NULL, false, &id));
    TEST_ASSERT_EQUAL_UINT32(0, id);
    /* and a valid lock action that isn't allowed with the option off: refused, nothing sent */
    id = 99u;
    TEST_ASSERT_EQUAL(HC_ERR_NOT_ALLOWED, hc_act_auto(&HC, "zb/porch", ACT_UNLOCK, 0, NULL, false, &id));
    TEST_ASSERT_EQUAL_UINT32(0, id);
    TEST_ASSERT_EQUAL_UINT32(0, hc_get(&HC, "zb/porch")->pending);

    /* a ref that is now a script: RUN fits it, and is never allowed */
    FAKE.devs[0].kind = (uint8_t)DEV_SCRIPT;
    hc_refresh(&HC);
    TEST_ASSERT_EQUAL(HC_ERR_NOT_ALLOWED, hc_act_auto(&HC, "zb/porch", ACT_RUN, 0, NULL, true, &id));
    /* a light whose ACT_ON carries brightness 0 */
    FAKE.devs[0].kind = (uint8_t)DEV_LIGHT;
    hc_refresh(&HC);
    TEST_ASSERT_EQUAL(HC_ERR_NOT_ALLOWED, hc_act_auto(&HC, "zb/porch", ACT_ON, 0, NULL, false, &id));
    TEST_ASSERT_EQUAL(HC_ERR_NOT_ALLOWED, hc_act_auto(&HC, "zb/porch", ACT_ON, 50, NULL, false, &id));
    TEST_ASSERT_EQUAL(HC_ERR_NOT_ALLOWED,
                      hc_act_auto(&HC, "zb/porch", ACT_SET_LEVEL, 40, NULL, false, &id));
    TEST_ASSERT_EQUAL_UINT32(0, id);
    TEST_ASSERT_EQUAL_UINT(sent, FAKE.act_calls); /* nothing reached the source */

    /* hc_act's other refusals, with id 0 */
    id = 99u;
    TEST_ASSERT_EQUAL(HC_ERR_UNKNOWN, hc_act_auto(&HC, "zb/nothing", ACT_OFF, -1, NULL, false, &id));
    TEST_ASSERT_EQUAL_UINT32(0, id);
    id = 99u;
    TEST_ASSERT_EQUAL(HC_ERR_UNKNOWN, hc_act_auto(&HC, NULL, ACT_OFF, -1, NULL, false, &id));
    TEST_ASSERT_EQUAL_UINT32(0, id);
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/porch", ACT_ON, -1, NULL)); /* a person acts */
    id = 99u;
    TEST_ASSERT_EQUAL(HC_ERR_BUSY, hc_act_auto(&HC, "zb/porch", ACT_OFF, -1, NULL, false, &id));
    TEST_ASSERT_EQUAL_UINT32(0, id);
    FAKE.act_returns = false;
    id = 99u;
    TEST_ASSERT_EQUAL(HC_ERR_OFFLINE, hc_act_auto(&HC, "zb/front", ACT_LOCK, 0, NULL, false, &id));
    TEST_ASSERT_EQUAL_UINT32(0, id); /* the source said no: nothing under way */
    TEST_ASSERT_EQUAL_UINT32(0, hc_get(&HC, "zb/front")->pending);
    FAKE.online = false;
    id = 99u;
    TEST_ASSERT_EQUAL(HC_ERR_OFFLINE, hc_act_auto(&HC, "zb/front", ACT_LOCK, 0, NULL, false, &id));
    TEST_ASSERT_EQUAL_UINT32(0, id);
    /* request_id may be NULL */
    FAKE.online = true;
    FAKE.act_returns = true;
    TEST_ASSERT_EQUAL(HC_OK, hc_act_auto(&HC, "zb/front", ACT_LOCK, 0, NULL, false, NULL));
}

/* HCTL-012: an automation's command whose device a refresh drops fails as "offline" with the id
 * hc_act_auto returned; a disconnect does the same. Each is told once. */
static void test_act_auto_dropped_row_fails_with_its_id(void)
{
    with_two_fake_devices();
    uint32_t porch = 0;
    uint32_t front = 0;
    TEST_ASSERT_EQUAL(HC_OK, hc_act_auto(&HC, "zb/porch", ACT_OFF, -1, NULL, false, &porch));
    TEST_ASSERT_EQUAL(HC_OK, hc_act_auto(&HC, "zb/front", ACT_LOCK, 0, NULL, false, &front));
    TEST_ASSERT_NOT_EQUAL(porch, front);

    /* the porch light leaves the list; the lock stays and stays pending */
    FAKE.devs[0] = FAKE.devs[1];
    FAKE.n = 1;
    hc_refresh(&HC);
    TEST_ASSERT_EQUAL_UINT(1, RESULTS);
    TEST_ASSERT_EQUAL_UINT32(porch, R_ID);
    TEST_ASSERT_EQUAL_STRING("zb/porch", R_REF);
    TEST_ASSERT_EQUAL_STRING("offline", R_ERR);
    TEST_ASSERT_FALSE(R_OK);
    TEST_ASSERT_EQUAL_UINT32(front, hc_get(&HC, "zb/front")->pending);
    hc_refresh(&HC); /* told once */
    TEST_ASSERT_EQUAL_UINT(1, RESULTS);

    /* the connection drops: the lock's command fails as offline with its id */
    FAKE.online = false;
    hc_refresh(&HC);
    TEST_ASSERT_EQUAL_UINT(2, RESULTS);
    TEST_ASSERT_EQUAL_UINT32(front, R_ID);
    TEST_ASSERT_EQUAL_STRING("zb/front", R_REF);
    TEST_ASSERT_EQUAL_STRING("offline", R_ERR);
}

/* R15: a cover's ACT_SET_LEVEL is open() in disguise for every target but fully closed - the HA
 * adapter sends it as set_position, so a position slider must ask for a code exactly when the
 * Open button would (HCTL-005), fully or partially; closing it (value 0) matches ACT_CLOSE and
 * asks for none, the explicit, written policy for closing a cover. */
static void test_needs_code_cover_set_level(void)
{
    TEST_ASSERT_FALSE_MESSAGE(hc_needs_code(DEV_COVER, ACT_SET_LEVEL, 0),
                              "fully closed (0) must match ACT_CLOSE: no code");
    TEST_ASSERT_TRUE_MESSAGE(hc_needs_code(DEV_COVER, ACT_SET_LEVEL, 100),
                             "fully open (100) must match ACT_OPEN: a code");
    TEST_ASSERT_TRUE_MESSAGE(hc_needs_code(DEV_COVER, ACT_SET_LEVEL, 1),
                             "the barest crack open still needs a code");
    TEST_ASSERT_TRUE_MESSAGE(hc_needs_code(DEV_COVER, ACT_SET_LEVEL, 50),
                             "partially open needs a code same as fully open");
    TEST_ASSERT_TRUE_MESSAGE(hc_needs_code(DEV_COVER, ACT_SET_LEVEL, -1),
                             "an unrecognised negative target is not known to be closed");
    /* a fan or a light's set-level is never security-relevant, whatever the value */
    TEST_ASSERT_FALSE(hc_needs_code(DEV_FAN, ACT_SET_LEVEL, 100));
    TEST_ASSERT_FALSE(hc_needs_code(DEV_LIGHT, ACT_SET_LEVEL, 100));
}

/* HCTL-005 (BL-179 question 3, kept 2026-10-07): a Position target is judged by its effect alone,
 * not by the cover's class, its reported position or the direction of travel. A cover that reports
 * 80 % open (a stale report: it may really be at 10 %) set to 20 % asks for the code Open asks for -
 * moving the slider down is still an absolute target that can open it. Only exactly 0 is fully
 * closed. A target outside 0..100 is for action validation to judge; it gains no exemption. */
static void test_needs_code_cover_position_below_reported(void)
{
    fake_add(&FAKE, "zb/blinds", DEV_COVER, "Blinds", "Living room", "open", true);
    FAKE.devs[0].values.flags = DEV_VAL_LEVEL | DEV_VAL_LEVEL_SET;
    FAKE.devs[0].values.level = 80;
    FAKE.devs[0].values.caps = DEV_CAP_KNOWN | DEV_CAP_STOP | DEV_CAP_POSITION;
    fake_add(&FAKE, "zb/garage", DEV_COVER, "Garage Door", "Outside", "open", true);
    FAKE.devs[1].values.flags = DEV_VAL_LEVEL;
    FAKE.devs[1].values.level = 80;
    TEST_ASSERT_TRUE(hc_register(&HC, &FAKE_OPS, &FAKE));
    hc_refresh(&HC);

    static const char *const REFS[] = {"zb/blinds", "zb/garage"};
    for (size_t i = 0; i < sizeof(REFS) / sizeof(REFS[0]); i++) {
        const hc_device_t *d = hc_get(&HC, REFS[i]);
        TEST_ASSERT_NOT_NULL(d);
        TEST_ASSERT_EQUAL_UINT8(80, d->info.values.level);
        const dev_kind_t kind = (dev_kind_t)d->info.kind;
        TEST_ASSERT_TRUE_MESSAGE(hc_needs_code(kind, ACT_SET_LEVEL, 20),
                                 "a positive target below the reported position must ask for a code");
        TEST_ASSERT_TRUE_MESSAGE(hc_needs_code(kind, ACT_SET_LEVEL, 79), "just below the report still asks");
        TEST_ASSERT_TRUE(hc_needs_code(kind, ACT_SET_LEVEL, 80)); /* the reported position itself */
        TEST_ASSERT_TRUE(hc_needs_code(kind, ACT_SET_LEVEL, 99));
        TEST_ASSERT_FALSE_MESSAGE(hc_needs_code(kind, ACT_SET_LEVEL, 0),
                                  "a target of exactly 0 is fully closed: no code");
        TEST_ASSERT_FALSE(hc_needs_code(kind, ACT_CLOSE, 0));
        TEST_ASSERT_FALSE(hc_needs_code(kind, ACT_STOP, 0));
        /* outside 0..100: no exemption */
        TEST_ASSERT_TRUE(hc_needs_code(kind, ACT_SET_LEVEL, 101));
        TEST_ASSERT_TRUE(hc_needs_code(kind, ACT_SET_LEVEL, INT32_MAX));
        TEST_ASSERT_TRUE(hc_needs_code(kind, ACT_SET_LEVEL, INT32_MIN));
    }
    /* asking is the platform's part (the engine checks the code first); what was asked is never
     * sent until then, and the registry carries a target of 0 as it is */
    TEST_ASSERT_EQUAL_UINT(0, FAKE.act_calls);
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/blinds", ACT_SET_LEVEL, 0, NULL));
    TEST_ASSERT_EQUAL_UINT(1, FAKE.act_calls);
    TEST_ASSERT_EQUAL(ACT_SET_LEVEL, FAKE.act_action);
    TEST_ASSERT_EQUAL_INT32(0, FAKE.act_value);
}

/* HCTL-004: the actions the UI may offer for each kind (the table of docs/features/24). */
static void test_action_valid_table(void)
{
    static const struct {
        dev_kind_t kind;
        dev_action_t allowed[5];
    } table[] = {
        {DEV_LIGHT, {ACT_ON, ACT_OFF, ACT_TOGGLE, ACT_SET_LEVEL, ACT_NONE}},
        {DEV_SWITCH, {ACT_ON, ACT_OFF, ACT_TOGGLE, ACT_NONE, ACT_NONE}},
        {DEV_FAN, {ACT_ON, ACT_OFF, ACT_SET_LEVEL, ACT_NONE, ACT_NONE}},
        {DEV_CLIMATE, {ACT_SET_TEMP, ACT_SET_MODE, ACT_NONE, ACT_NONE, ACT_NONE}},
        {DEV_COVER, {ACT_OPEN, ACT_CLOSE, ACT_STOP, ACT_SET_LEVEL, ACT_NONE}},
        {DEV_LOCK, {ACT_LOCK, ACT_UNLOCK, ACT_NONE, ACT_NONE, ACT_NONE}},
        {DEV_VALVE, {ACT_OPEN, ACT_CLOSE, ACT_NONE, ACT_NONE, ACT_NONE}},
        {DEV_SCENE, {ACT_RUN, ACT_NONE, ACT_NONE, ACT_NONE, ACT_NONE}},
        {DEV_SCRIPT, {ACT_RUN, ACT_NONE, ACT_NONE, ACT_NONE, ACT_NONE}},
    };
    for (size_t t = 0; t < sizeof(table) / sizeof(table[0]); t++) {
        for (int a = ACT_NONE; a < ACT_COUNT; a++) {
            bool expect = false;
            for (size_t i = 0; i < 5 && table[t].allowed[i] != ACT_NONE; i++) {
                expect = expect || table[t].allowed[i] == (dev_action_t)a;
            }
            TEST_ASSERT_EQUAL_MESSAGE(expect, hc_action_valid(table[t].kind, (dev_action_t)a),
                                      "hc_action_valid disagrees with docs/features/24");
        }
    }
    /* nothing at all for a device of no kind */
    for (int a = ACT_NONE; a < ACT_COUNT; a++) {
        TEST_ASSERT_FALSE(hc_action_valid(DEV_NONE, (dev_action_t)a));
    }
    TEST_ASSERT_FALSE(hc_action_valid(DEV_LIGHT, ACT_SET_TEMP));
    TEST_ASSERT_FALSE(hc_action_valid(DEV_LOCK, ACT_OPEN));
    TEST_ASSERT_FALSE(hc_action_valid(DEV_SCENE, ACT_OFF));
}

/* ---- actions and their results (HCTL-006) ----------------------------------------------------- */

/* HCTL-006: the action reaches the source with a request id, the device shows it as pending,
 * and the answer clears it and tells the UI. */
static void test_act_then_result_ok(void)
{
    with_two_fake_devices();

    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/porch", ACT_SET_LEVEL, 40, NULL));
    TEST_ASSERT_EQUAL_UINT(1, FAKE.act_calls);
    TEST_ASSERT_EQUAL_STRING("zb/porch", FAKE.act_ref);
    TEST_ASSERT_EQUAL(ACT_SET_LEVEL, FAKE.act_action);
    TEST_ASSERT_EQUAL_INT32(40, FAKE.act_value);
    const uint32_t id = FAKE.act_id;
    TEST_ASSERT_NOT_EQUAL(0, id);
    TEST_ASSERT_EQUAL_UINT32(id, hc_get(&HC, "zb/porch")->pending);

    const unsigned changes = CHANGES;
    hc_sink(&HC)->result(&HC, id, true, "", false);
    TEST_ASSERT_EQUAL_UINT(1, RESULTS);
    TEST_ASSERT_TRUE(R_OK);
    TEST_ASSERT_EQUAL_STRING("zb/porch", R_REF);
    TEST_ASSERT_EQUAL_STRING("", R_ERR);
    TEST_ASSERT_EQUAL_UINT(changes + 1u, CHANGES);

    const hc_device_t *d = hc_get(&HC, "zb/porch");
    TEST_ASSERT_EQUAL_UINT32(0, d->pending);
    TEST_ASSERT_FALSE(d->failed);
    TEST_ASSERT_EQUAL_STRING("", d->error);

    /* the next action gets its own id */
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/porch", ACT_OFF, 0, NULL));
    TEST_ASSERT_NOT_EQUAL(id, FAKE.act_id);
}

/* HCTL-006: a failure leaves the reason on the device row and hands it to the UI. */
static void test_act_result_failure(void)
{
    with_two_fake_devices();
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/front", ACT_UNLOCK, 0, NULL));

    hc_sink(&HC)->result(&HC, FAKE.act_id, false, "not_allowed", false);
    TEST_ASSERT_EQUAL_UINT(1, RESULTS);
    TEST_ASSERT_FALSE(R_OK);
    TEST_ASSERT_EQUAL_STRING("zb/front", R_REF);
    TEST_ASSERT_EQUAL_STRING("not_allowed", R_ERR);

    const hc_device_t *d = hc_get(&HC, "zb/front");
    TEST_ASSERT_EQUAL_UINT32(0, d->pending);
    TEST_ASSERT_TRUE(d->failed);
    TEST_ASSERT_EQUAL_STRING("not_allowed", d->error);

    /* a failure with no word of its own still says something */
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/front", ACT_LOCK, 0, NULL));
    hc_sink(&HC)->result(&HC, FAKE.act_id, false, NULL, false);
    TEST_ASSERT_EQUAL_STRING("failed", hc_get(&HC, "zb/front")->error);

    /* the next action clears the last failure */
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/front", ACT_LOCK, 0, NULL));
    d = hc_get(&HC, "zb/front");
    TEST_ASSERT_FALSE(d->failed);
    TEST_ASSERT_EQUAL_STRING("", d->error);
}

/* HCTL-006: one action at a time per device; an unknown device, an offline one and an action
 * that doesn't fit are all refused before anything is sent. */
static void test_act_refusals(void)
{
    with_two_fake_devices();

    TEST_ASSERT_EQUAL(HC_ERR_UNKNOWN, hc_act(&HC, "zb/nothing", ACT_ON, 0, NULL));
    TEST_ASSERT_EQUAL(HC_ERR_UNKNOWN, hc_act(&HC, NULL, ACT_ON, 0, NULL));
    TEST_ASSERT_EQUAL(HC_ERR_ARG, hc_act(&HC, "zb/porch", ACT_LOCK, 0, NULL));
    TEST_ASSERT_EQUAL(HC_ERR_ARG, hc_act(&HC, "zb/front", ACT_SET_TEMP, 2150, NULL));
    TEST_ASSERT_EQUAL_UINT(0, FAKE.act_calls);

    /* busy: an action is already under way on that device (another device still works) */
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/porch", ACT_ON, -1, NULL));
    TEST_ASSERT_EQUAL(HC_ERR_BUSY, hc_act(&HC, "zb/porch", ACT_OFF, 0, NULL));
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/front", ACT_LOCK, 0, NULL));
    TEST_ASSERT_EQUAL_UINT(2, FAKE.act_calls);

    /* the device itself says it is unreachable */
    HC.devices[0].pending = 0;
    HC.devices[0].info.online = false;
    TEST_ASSERT_EQUAL(HC_ERR_OFFLINE, hc_act(&HC, "zb/porch", ACT_ON, 0, NULL));
    HC.devices[0].info.online = true;

    /* the whole source is unreachable */
    FAKE.online = false;
    TEST_ASSERT_EQUAL(HC_ERR_OFFLINE, hc_act(&HC, "zb/porch", ACT_ON, 0, NULL));
    TEST_ASSERT_EQUAL_UINT(2, FAKE.act_calls);
}

/* HCTL-006: a source that can't take the action right now leaves no pending action behind. */
static void test_act_refused_by_the_source(void)
{
    with_two_fake_devices();
    FAKE.act_returns = false;

    TEST_ASSERT_EQUAL(HC_ERR_OFFLINE, hc_act(&HC, "zb/porch", ACT_ON, 0, NULL));
    TEST_ASSERT_EQUAL_UINT(1, FAKE.act_calls);
    TEST_ASSERT_EQUAL_UINT32(0, hc_get(&HC, "zb/porch")->pending);
    TEST_ASSERT_EQUAL_UINT(0, RESULTS);

    /* so the next one goes through */
    FAKE.act_returns = true;
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/porch", ACT_ON, 0, NULL));
    TEST_ASSERT_NOT_EQUAL(0, hc_get(&HC, "zb/porch")->pending);
}

/* ---- a pending action's lifetime: timeout, disconnect, duplicates, wraparound (R14 / HCTL-012)
 * -------------------------------------------------------------------------------------------- */

/* R14: a reply that never comes must not leave a device busy forever - hc_tick fails it once
 * its deadline passes (the clock starts on the first tick after hc_act, since hc_act has no
 * time of its own), and the stale reply that eventually turns up is ignored rather than
 * completing whatever the device has moved on to. */
static void test_act_times_out_and_recovers(void)
{
    with_two_fake_devices();
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/porch", ACT_ON, -1, NULL));
    const uint32_t first_id = hc_get(&HC, "zb/porch")->pending;
    TEST_ASSERT_NOT_EQUAL(0, first_id);

    hc_tick(&HC, 1000);
    TEST_ASSERT_EQUAL_UINT32(first_id, hc_get(&HC, "zb/porch")->pending); /* clock just started */
    TEST_ASSERT_EQUAL_UINT(0, RESULTS);

    hc_tick(&HC, 1000 + HC_ACT_TIMEOUT_MS - 1u);
    TEST_ASSERT_EQUAL_UINT32(first_id, hc_get(&HC, "zb/porch")->pending); /* short of the deadline */
    TEST_ASSERT_EQUAL_UINT(0, RESULTS);

    hc_tick(&HC, 1000 + HC_ACT_TIMEOUT_MS); /* the deadline: no reply ever came */
    TEST_ASSERT_EQUAL_UINT(1, RESULTS);
    TEST_ASSERT_FALSE(R_OK);
    TEST_ASSERT_EQUAL_STRING("zb/porch", R_REF);
    TEST_ASSERT_EQUAL_STRING("timeout", R_ERR);
    const hc_device_t *d = hc_get(&HC, "zb/porch");
    TEST_ASSERT_EQUAL_UINT32(0, d->pending);
    TEST_ASSERT_TRUE(d->failed);
    TEST_ASSERT_EQUAL_STRING("timeout", d->error);

    /* never busy indefinitely: a deliberate retry goes through */
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/porch", ACT_ON, -1, NULL));
    const uint32_t second_id = hc_get(&HC, "zb/porch")->pending;
    TEST_ASSERT_NOT_EQUAL(first_id, second_id);

    /* the original reply finally turns up, long after it timed out: an old reply must not
     * complete a newer command */
    const unsigned results = RESULTS;
    hc_sink(&HC)->result(&HC, first_id, true, "", false);
    TEST_ASSERT_EQUAL_UINT(results, RESULTS);
    TEST_ASSERT_EQUAL_UINT32(second_id, hc_get(&HC, "zb/porch")->pending);

    /* a tick with nothing due (zb/front has no pending action, zb/porch isn't due yet) is a
     * no-op */
    hc_tick(&HC, 1000 + HC_ACT_TIMEOUT_MS);
    TEST_ASSERT_EQUAL_UINT(results, RESULTS);
}

/* R14: a source going offline while a command is in flight must resolve it as soon as the next
 * refresh notices (the platform's normal path: sink.changed -> hc_refresh) - not leave the
 * device rejecting every action as busy - and reconnecting must not replay anything on its own,
 * only let a person's own retry through. */
static void test_act_disconnect_invalidates_pending(void)
{
    with_two_fake_devices();
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/porch", ACT_ON, -1, NULL));
    const uint32_t id = hc_get(&HC, "zb/porch")->pending;
    TEST_ASSERT_NOT_EQUAL(0, id);

    FAKE.online = false;
    hc_refresh(&HC);
    TEST_ASSERT_EQUAL_UINT(1, RESULTS);
    TEST_ASSERT_FALSE(R_OK);
    TEST_ASSERT_EQUAL_STRING("zb/porch", R_REF);
    TEST_ASSERT_EQUAL_STRING("offline", R_ERR);
    const hc_device_t *d = hc_get(&HC, "zb/porch");
    TEST_ASSERT_EQUAL_UINT32(0, d->pending);
    TEST_ASSERT_TRUE(d->failed);
    TEST_ASSERT_EQUAL_STRING("offline", d->error);

    /* still offline: refused as offline, not busy - and nothing was replayed on its own */
    TEST_ASSERT_EQUAL(HC_ERR_OFFLINE, hc_act(&HC, "zb/porch", ACT_ON, -1, NULL));
    TEST_ASSERT_EQUAL_UINT(1, FAKE.act_calls); /* only the original call ever reached the source */

    /* back online: a fresh refresh doesn't resurrect the old pending action either */
    FAKE.online = true;
    hc_refresh(&HC);
    TEST_ASSERT_EQUAL_UINT32(0, hc_get(&HC, "zb/porch")->pending);

    /* a person's deliberate retry goes through */
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/porch", ACT_ON, -1, NULL));
    TEST_ASSERT_EQUAL_UINT(2, FAKE.act_calls);
    const uint32_t second_id = hc_get(&HC, "zb/porch")->pending;
    TEST_ASSERT_NOT_EQUAL(id, second_id);

    /* the original, long-lost reply turns up: it must not complete the new command */
    const unsigned results = RESULTS;
    hc_sink(&HC)->result(&HC, id, true, "", false);
    TEST_ASSERT_EQUAL_UINT(results, RESULTS);
    TEST_ASSERT_EQUAL_UINT32(second_id, hc_get(&HC, "zb/porch")->pending);
}

/* R14 (the reported repro): a lost reply across a disconnect/reconnect on the real Home
 * Assistant source must not leave the device busy forever - mirrors
 * a probe that showed "After reconnect pending=1 next action=3 (HC_ERR_BUSY=3)". */
static void test_ha_disconnect_reconnect_recovers(void)
{
    dsrc_ha_link(&HA, true);
    dsrc_ha_catalog_add(&HA, "light.test", "Test", "Room", "light", "off", NULL);
    TEST_ASSERT_TRUE(hc_register(&HC, dsrc_ha_ops(), &HA));
    hc_refresh(&HC);

    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "light.test", ACT_ON, -1, NULL));

    dsrc_ha_link(&HA, false);
    hc_refresh(&HC);
    dsrc_ha_link(&HA, true);
    hc_refresh(&HC);

    TEST_ASSERT_EQUAL_UINT32(0, hc_get(&HC, "light.test")->pending);
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "light.test", ACT_OFF, -1, NULL));
}

/* R14: a duplicate reply (the same request id answered twice) must not re-fire the result or
 * reach into whatever the device has moved on to since. */
static void test_act_duplicate_reply_ignored(void)
{
    with_two_fake_devices();
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/porch", ACT_ON, -1, NULL));
    const uint32_t id = FAKE.act_id;

    hc_sink(&HC)->result(&HC, id, true, "", false);
    TEST_ASSERT_EQUAL_UINT(1, RESULTS);

    /* the same answer again: nothing left to complete */
    hc_sink(&HC)->result(&HC, id, true, "", false);
    TEST_ASSERT_EQUAL_UINT(1, RESULTS);

    /* nor can it reach the device's next, unrelated action */
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/porch", ACT_OFF, 0, NULL));
    const uint32_t second_id = FAKE.act_id;
    TEST_ASSERT_NOT_EQUAL(id, second_id);
    hc_sink(&HC)->result(&HC, id, false, "late", false);
    TEST_ASSERT_EQUAL_UINT(1, RESULTS);
    TEST_ASSERT_EQUAL_UINT32(second_id, hc_get(&HC, "zb/porch")->pending);
}

/* R14: ids stay unique across a forced wraparound (0 is skipped, "no request under way"), and a
 * stale reply from before the wrap completes only the device that is actually waiting on it -
 * never the device that wrapped around to reuse that same small number. */
static void test_act_request_id_wraparound(void)
{
    with_two_fake_devices();
    HC.next_request = 0xFFFFFFFEu; /* one hc_act away from wrapping past 0 back to 1 */

    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/porch", ACT_ON, -1, NULL));
    const uint32_t last_id = hc_get(&HC, "zb/porch")->pending;
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, last_id); /* the last id before the wrap */

    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/front", ACT_LOCK, 0, NULL));
    const uint32_t wrapped_id = hc_get(&HC, "zb/front")->pending;
    TEST_ASSERT_EQUAL_UINT32(1u, wrapped_id); /* wrapped, skipping 0 ("no request under way") */
    TEST_ASSERT_NOT_EQUAL(last_id, wrapped_id);

    hc_sink(&HC)->result(&HC, last_id, true, "", false);
    TEST_ASSERT_EQUAL_UINT(1, RESULTS);
    TEST_ASSERT_EQUAL_STRING("zb/porch", R_REF);
    TEST_ASSERT_EQUAL_UINT32(wrapped_id, hc_get(&HC, "zb/front")->pending); /* untouched */

    hc_sink(&HC)->result(&HC, wrapped_id, true, "", false);
    TEST_ASSERT_EQUAL_UINT(2, RESULTS);
    TEST_ASSERT_EQUAL_STRING("zb/front", R_REF);
    TEST_ASSERT_EQUAL_UINT32(0, hc_get(&HC, "zb/front")->pending);
}

/* ---- Home Assistant as a device source -------------------------------------------------------- */

/* HCTL-001: the domains the panel can control, and nothing else. */
static void test_ha_kind_table(void)
{
    TEST_ASSERT_EQUAL(DEV_LIGHT, dsrc_ha_kind("light"));
    TEST_ASSERT_EQUAL(DEV_SWITCH, dsrc_ha_kind("switch"));
    TEST_ASSERT_EQUAL(DEV_SWITCH, dsrc_ha_kind("input_boolean"));
    TEST_ASSERT_EQUAL(DEV_LOCK, dsrc_ha_kind("lock"));
    TEST_ASSERT_EQUAL(DEV_CLIMATE, dsrc_ha_kind("climate"));
    TEST_ASSERT_EQUAL(DEV_COVER, dsrc_ha_kind("cover"));
    TEST_ASSERT_EQUAL(DEV_FAN, dsrc_ha_kind("fan"));
    TEST_ASSERT_EQUAL(DEV_VALVE, dsrc_ha_kind("valve"));
    TEST_ASSERT_EQUAL(DEV_SCENE, dsrc_ha_kind("scene"));
    TEST_ASSERT_EQUAL(DEV_SCRIPT, dsrc_ha_kind("script"));
    /* never from the panel: no alarm loops, and sensors are zones, not devices */
    TEST_ASSERT_EQUAL(DEV_NONE, dsrc_ha_kind("alarm_control_panel"));
    TEST_ASSERT_EQUAL(DEV_NONE, dsrc_ha_kind("siren"));
    TEST_ASSERT_EQUAL(DEV_NONE, dsrc_ha_kind("binary_sensor"));
    TEST_ASSERT_EQUAL(DEV_NONE, dsrc_ha_kind("automation"));
    TEST_ASSERT_EQUAL(DEV_NONE, dsrc_ha_kind(""));
    TEST_ASSERT_EQUAL(DEV_NONE, dsrc_ha_kind(NULL));
}

/* HCTL-001: the protocol's action word for each pair, and NULL for every pair the registry
 * refuses - the two tables must not drift apart. */
static void test_ha_action_words(void)
{
    for (int k = DEV_NONE; k < DEV_KIND_COUNT; k++) {
        for (int a = ACT_NONE; a < ACT_COUNT; a++) {
            const char *word = dsrc_ha_action_word((dev_kind_t)k, (dev_action_t)a);
            TEST_ASSERT_EQUAL_MESSAGE(hc_action_valid((dev_kind_t)k, (dev_action_t)a),
                                      word != NULL,
                                      "dsrc_ha_action_word and hc_action_valid disagree");
        }
    }
    TEST_ASSERT_EQUAL_STRING("turn_on", dsrc_ha_action_word(DEV_LIGHT, ACT_ON));
    TEST_ASSERT_EQUAL_STRING("turn_off", dsrc_ha_action_word(DEV_LIGHT, ACT_OFF));
    TEST_ASSERT_EQUAL_STRING("toggle", dsrc_ha_action_word(DEV_SWITCH, ACT_TOGGLE));
    TEST_ASSERT_EQUAL_STRING("lock", dsrc_ha_action_word(DEV_LOCK, ACT_LOCK));
    TEST_ASSERT_EQUAL_STRING("unlock", dsrc_ha_action_word(DEV_LOCK, ACT_UNLOCK));
    TEST_ASSERT_EQUAL_STRING("open", dsrc_ha_action_word(DEV_COVER, ACT_OPEN));
    TEST_ASSERT_EQUAL_STRING("close", dsrc_ha_action_word(DEV_VALVE, ACT_CLOSE));
    TEST_ASSERT_EQUAL_STRING("stop", dsrc_ha_action_word(DEV_COVER, ACT_STOP));
    TEST_ASSERT_EQUAL_STRING("turn_on", dsrc_ha_action_word(DEV_LIGHT, ACT_SET_LEVEL));
    TEST_ASSERT_EQUAL_STRING("set_percentage", dsrc_ha_action_word(DEV_FAN, ACT_SET_LEVEL));
    TEST_ASSERT_EQUAL_STRING("set_position", dsrc_ha_action_word(DEV_COVER, ACT_SET_LEVEL));
    TEST_ASSERT_EQUAL_STRING("set_temperature", dsrc_ha_action_word(DEV_CLIMATE, ACT_SET_TEMP));
    TEST_ASSERT_EQUAL_STRING("set_hvac_mode", dsrc_ha_action_word(DEV_CLIMATE, ACT_SET_MODE));
    TEST_ASSERT_EQUAL_STRING("turn_on", dsrc_ha_action_word(DEV_SCENE, ACT_RUN));
    TEST_ASSERT_EQUAL_STRING("turn_on", dsrc_ha_action_word(DEV_SCRIPT, ACT_RUN));

    TEST_ASSERT_NULL(dsrc_ha_action_word(DEV_LIGHT, ACT_LOCK));
    TEST_ASSERT_NULL(dsrc_ha_action_word(DEV_LOCK, ACT_OPEN));
    TEST_ASSERT_NULL(dsrc_ha_action_word(DEV_VALVE, ACT_SET_LEVEL));
    TEST_ASSERT_NULL(dsrc_ha_action_word(DEV_SWITCH, ACT_SET_LEVEL));
    TEST_ASSERT_NULL(dsrc_ha_action_word(DEV_SCENE, ACT_ON));
    TEST_ASSERT_NULL(dsrc_ha_action_word(DEV_NONE, ACT_ON));
    TEST_ASSERT_NULL(dsrc_ha_action_word(DEV_LIGHT, ACT_NONE));
}

/* HCTL-002: the catalog the integration sends becomes the device list; a repeat replaces the
 * entry, a domain the panel can't control is ignored, and a page 0 starts fresh. */
static void test_ha_catalog(void)
{
    dsrc_ha_link(&HA, true);
    dsrc_ha_catalog_begin(&HA);
    dsrc_ha_catalog_add(&HA, "light.porch", "Porch", "Outside", "light", "on", NULL);
    dsrc_ha_catalog_add(&HA, "sensor.temp", "Hall temperature", "Hall", "sensor", "21.5", NULL);
    dsrc_ha_catalog_add(&HA, "lock.front", "Front Door", "Hall", "lock", "locked", NULL);
    dsrc_ha_catalog_add(&HA, "", "no id", "", "light", "on", NULL);
    TEST_ASSERT_EQUAL_size_t(2, HA.n);
    TEST_ASSERT_EQUAL_STRING("light.porch", HA.devices[0].ref);
    TEST_ASSERT_EQUAL_STRING("Porch", HA.devices[0].name);
    TEST_ASSERT_EQUAL_STRING("Outside", HA.devices[0].area);
    TEST_ASSERT_EQUAL_STRING("on", HA.devices[0].state);
    TEST_ASSERT_EQUAL_UINT8(DEV_LIGHT, HA.devices[0].kind);
    TEST_ASSERT_EQUAL_UINT8(DSRC_HA, HA.devices[0].source);
    TEST_ASSERT_TRUE(HA.devices[0].online);
    TEST_ASSERT_EQUAL_UINT8(DEV_LOCK, HA.devices[1].kind);
    TEST_ASSERT_TRUE(CHANGES > 0);

    /* the same entity again: replaced in place, not doubled */
    dsrc_ha_catalog_add(&HA, "light.porch", "Porch Light", "Garden", "light", "off", NULL);
    TEST_ASSERT_EQUAL_size_t(2, HA.n);
    TEST_ASSERT_EQUAL_STRING("Porch Light", HA.devices[0].name);
    TEST_ASSERT_EQUAL_STRING("Garden", HA.devices[0].area);
    TEST_ASSERT_EQUAL_STRING("off", HA.devices[0].state);

    /* no name of its own: the entity_id is what the screen has to show */
    dsrc_ha_catalog_add(&HA, "scene.evening", "", "", "scene", "", NULL);
    TEST_ASSERT_EQUAL_STRING("scene.evening", HA.devices[2].name);

    /* a fresh page 0 replaces the whole catalog */
    dsrc_ha_catalog_begin(&HA);
    TEST_ASSERT_EQUAL_size_t(0, HA.n);
}

/* HCTL-002 / HCTL-006: state pushes change the words; the link going down greys everything and
 * coming back restores it. */
static void test_ha_state_and_link(void)
{
    dsrc_ha_link(&HA, true);
    dsrc_ha_catalog_add(&HA, "light.porch", "Porch", "Outside", "light", "on", NULL);
    dsrc_ha_catalog_add(&HA, "climate.hall", "Hall", "Hall", "climate", "heat", NULL);

    dsrc_ha_state(&HA, "light.porch", "off", true, NULL);
    TEST_ASSERT_EQUAL_STRING("off", HA.devices[0].state);
    TEST_ASSERT_TRUE(HA.devices[0].online);

    /* unavailable in Home Assistant: offline, with no stale word left behind */
    dsrc_ha_state(&HA, "light.porch", "off", false, NULL);
    TEST_ASSERT_FALSE(HA.devices[0].online);
    TEST_ASSERT_EQUAL_STRING("", HA.devices[0].state);
    dsrc_ha_state(&HA, "light.porch", "on", true, NULL);
    TEST_ASSERT_TRUE(HA.devices[0].online);

    /* an entity outside the control catalog is not ours */
    const unsigned changes = CHANGES;
    dsrc_ha_state(&HA, "binary_sensor.back_door", "on", true, NULL);
    TEST_ASSERT_EQUAL_size_t(2, HA.n);
    TEST_ASSERT_EQUAL_UINT(changes, CHANGES);

    /* the link drops: everything goes offline and the UI hears about it */
    dsrc_ha_link(&HA, false);
    TEST_ASSERT_EQUAL_UINT(changes + 1u, CHANGES);
    TEST_ASSERT_FALSE(HA.devices[0].online);
    TEST_ASSERT_FALSE(HA.devices[1].online);
    TEST_ASSERT_FALSE(dsrc_ha_ops()->online(&HA));

    dsrc_ha_link(&HA, true);
    TEST_ASSERT_TRUE(HA.devices[0].online);
    TEST_ASSERT_TRUE(HA.devices[1].online);
    TEST_ASSERT_TRUE(dsrc_ha_ops()->online(&HA));

    /* while it is down a state push can't bring a device back on its own */
    dsrc_ha_link(&HA, false);
    dsrc_ha_state(&HA, "light.porch", "on", true, NULL);
    TEST_ASSERT_FALSE(HA.devices[0].online);
}

/* HCTL-006: what goes on the wire for each action (docs/features/23 protocol.md, `call`). */
static void test_ha_act_formats_the_call(void)
{
    dsrc_ha_link(&HA, true);
    dsrc_ha_catalog_add(&HA, "light.porch", "Porch", "Outside", "light", "on", NULL);
    dsrc_ha_catalog_add(&HA, "fan.study", "Study", "Study", "fan", "off", NULL);
    dsrc_ha_catalog_add(&HA, "cover.garage", "Garage", "Garage", "cover", "closed", NULL);
    dsrc_ha_catalog_add(&HA, "climate.hall", "Hall", "Hall", "climate", "heat", NULL);
    dsrc_ha_catalog_add(&HA, "lock.front", "Front", "Hall", "lock", "locked", NULL);
    const device_source_ops_t *ops = dsrc_ha_ops();

    /* a light's brightness rides along with turn_on */
    TEST_ASSERT_TRUE(ops->act(&HA, "light.porch", ACT_SET_LEVEL, 40, NULL, 7));
    TEST_ASSERT_EQUAL_STRING("light.porch", SEND.entity);
    TEST_ASSERT_EQUAL_STRING("turn_on", SEND.action);
    TEST_ASSERT_TRUE(SEND.has_value);
    TEST_ASSERT_EQUAL_INT32(40, SEND.value);
    TEST_ASSERT_EQUAL_UINT32(7, SEND.request_id);

    /* turn_on with a brightness, and turn_on plain */
    TEST_ASSERT_TRUE(ops->act(&HA, "light.porch", ACT_ON, 80, NULL, 8));
    TEST_ASSERT_EQUAL_STRING("turn_on", SEND.action);
    TEST_ASSERT_TRUE(SEND.has_value);
    TEST_ASSERT_EQUAL_INT32(80, SEND.value);
    TEST_ASSERT_TRUE(ops->act(&HA, "light.porch", ACT_ON, -1, NULL, 9));
    TEST_ASSERT_FALSE(SEND.has_value);
    TEST_ASSERT_TRUE(ops->act(&HA, "light.porch", ACT_OFF, 0, NULL, 10));
    TEST_ASSERT_EQUAL_STRING("turn_off", SEND.action);
    TEST_ASSERT_FALSE(SEND.has_value);

    TEST_ASSERT_TRUE(ops->act(&HA, "fan.study", ACT_SET_LEVEL, 60, NULL, 11));
    TEST_ASSERT_EQUAL_STRING("set_percentage", SEND.action);
    TEST_ASSERT_TRUE(SEND.has_value);
    TEST_ASSERT_EQUAL_INT32(60, SEND.value);

    TEST_ASSERT_TRUE(ops->act(&HA, "cover.garage", ACT_SET_LEVEL, 100, NULL, 12));
    TEST_ASSERT_EQUAL_STRING("set_position", SEND.action);
    TEST_ASSERT_TRUE(ops->act(&HA, "cover.garage", ACT_OPEN, 0, NULL, 13));
    TEST_ASSERT_EQUAL_STRING("open", SEND.action);
    TEST_ASSERT_FALSE(SEND.has_value);

    /* the setpoint in hundredths, and a mode by name */
    TEST_ASSERT_TRUE(ops->act(&HA, "climate.hall", ACT_SET_TEMP, 2150, NULL, 14));
    TEST_ASSERT_EQUAL_STRING("set_temperature", SEND.action);
    TEST_ASSERT_TRUE(SEND.has_value);
    TEST_ASSERT_EQUAL_INT32(2150, SEND.value);
    TEST_ASSERT_TRUE(ops->act(&HA, "climate.hall", ACT_SET_MODE, 0, "heat", 15));
    TEST_ASSERT_EQUAL_STRING("set_hvac_mode", SEND.action);
    TEST_ASSERT_EQUAL_STRING("heat", SEND.mode);
    TEST_ASSERT_FALSE(SEND.has_value);

    TEST_ASSERT_TRUE(ops->act(&HA, "lock.front", ACT_UNLOCK, 0, NULL, 16));
    TEST_ASSERT_EQUAL_STRING("unlock", SEND.action);

    /* nothing sensible to send: nothing sent */
    const unsigned calls = SEND.calls;
    TEST_ASSERT_FALSE(ops->act(&HA, "lock.front", ACT_OPEN, 0, NULL, 17));
    TEST_ASSERT_FALSE(ops->act(&HA, "light.nowhere", ACT_ON, 0, NULL, 18));
    TEST_ASSERT_EQUAL_UINT(calls, SEND.calls);

    /* the client said no, or there is nobody to send through */
    SEND.returns = false;
    TEST_ASSERT_FALSE(ops->act(&HA, "light.porch", ACT_ON, 0, NULL, 19));
    dsrc_ha_set_sender(&HA, NULL, NULL);
    TEST_ASSERT_FALSE(ops->act(&HA, "light.porch", ACT_ON, 0, NULL, 20));
    TEST_ASSERT_EQUAL_UINT(calls + 1u, SEND.calls);
}

/* HCTL-001: the source's list is what the registry merges. */
static void test_ha_list_through_the_registry(void)
{
    dsrc_ha_link(&HA, true);
    dsrc_ha_catalog_add(&HA, "light.porch", "Porch", "Outside", "light", "on", NULL);
    dsrc_ha_catalog_add(&HA, "lock.front", "Front", "Hall", "lock", "locked", NULL);
    fake_add(&FAKE, "zb/fan", DEV_FAN, "Study Fan", "Study", "off", true);

    TEST_ASSERT_TRUE(hc_register(&HC, dsrc_ha_ops(), &HA));
    TEST_ASSERT_TRUE(hc_register(&HC, &FAKE_OPS, &FAKE));
    hc_refresh(&HC);

    TEST_ASSERT_EQUAL_size_t(3, HC.n_devices);
    TEST_ASSERT_EQUAL_UINT8(DSRC_HA, hc_get(&HC, "light.porch")->info.source);
    TEST_ASSERT_EQUAL_UINT8(DSRC_ZIGBEE, hc_get(&HC, "zb/fan")->info.source);
}

/* HCTL-006: the whole way round - the screen acts on a Home Assistant device, the call goes
 * out, the client answers by request id and the row comes back clean. */
static void test_end_to_end_through_home_assistant(void)
{
    dsrc_ha_link(&HA, true);
    dsrc_ha_catalog_begin(&HA);
    dsrc_ha_catalog_add(&HA, "lock.front", "Front Door", "Hall", "lock", "locked", NULL);
    TEST_ASSERT_TRUE(hc_register(&HC, dsrc_ha_ops(), &HA));
    hc_refresh(&HC);

    const hc_device_t *d = hc_get(&HC, "lock.front");
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_TRUE(hc_needs_code((dev_kind_t)d->info.kind, ACT_UNLOCK, 0)); /* HCTL-005 */

    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "lock.front", ACT_UNLOCK, 0, NULL));
    TEST_ASSERT_EQUAL_UINT(1, SEND.calls);
    TEST_ASSERT_EQUAL_STRING("lock.front", SEND.entity);
    TEST_ASSERT_EQUAL_STRING("unlock", SEND.action);
    TEST_ASSERT_FALSE(SEND.has_value);
    TEST_ASSERT_EQUAL_UINT32(hc_get(&HC, "lock.front")->pending, SEND.request_id);

    /* the platform hands the client's call_result to the source, which passes it on by id */
    dsrc_ha_result(&HA, SEND.request_id, true, "");
    TEST_ASSERT_EQUAL_UINT(1, RESULTS);
    TEST_ASSERT_TRUE(R_OK);
    TEST_ASSERT_EQUAL_STRING("lock.front", R_REF);

    d = hc_get(&HC, "lock.front");
    TEST_ASSERT_EQUAL_UINT32(0, d->pending);
    TEST_ASSERT_FALSE(d->failed);
    TEST_ASSERT_EQUAL_STRING("", d->error);

    /* and the new state arrives on the ordinary feed */
    dsrc_ha_state(&HA, "lock.front", "unlocked", true, NULL);
    hc_refresh(&HC);
    TEST_ASSERT_EQUAL_STRING("unlocked", hc_get(&HC, "lock.front")->info.state);
}

/* ---- a device's values (BL-171) ------------------------------------------------------------ */

static dev_values_t values(uint8_t flags, uint8_t level, uint8_t modes, int32_t setpoint,
                           int32_t temp)
{
    dev_values_t v;
    memset(&v, 0, sizeof(v));
    v.flags = flags;
    v.level = level;
    v.modes = modes;
    v.setpoint = setpoint;
    v.temp = temp;
    return v;
}

/* HCTL-002: a catalog entity's values are kept with it - each only for the kinds that show it and
 * within its range; none known stays none (a client that sends no values works as before). */
static void test_ha_values_from_the_catalog(void)
{
    dsrc_ha_link(&HA, true);
    const dev_values_t lit = values(DEV_VAL_LEVEL | DEV_VAL_SETPOINT, 40, DEV_MODE_HEAT, 2150, 0);
    dsrc_ha_catalog_add(&HA, "light.porch", "Porch", "Outside", "light", "on", &lit);
    const dev_values_t hall = values(DEV_VAL_SETPOINT | DEV_VAL_TEMP | DEV_VAL_LEVEL, 50,
                                     DEV_MODE_OFF | DEV_MODE_HEAT | DEV_MODE_HEAT_COOL | 0x80u,
                                     2150, 1975);
    dsrc_ha_catalog_add(&HA, "climate.hall", "Hall", "Hall", "climate", "heat", &hall);
    const dev_values_t fan = values(DEV_VAL_LEVEL, 67, 0, 0, 0);
    dsrc_ha_catalog_add(&HA, "fan.study", "Study", "Study", "fan", "on", &fan);
    const dev_values_t lock = values(DEV_VAL_LEVEL | DEV_VAL_TEMP, 100, DEV_MODE_COOL, 0, 2000);
    dsrc_ha_catalog_add(&HA, "lock.front", "Front", "Hall", "lock", "locked", &lock);
    dsrc_ha_catalog_add(&HA, "cover.garage", "Garage", "Garage", "cover", "closed", NULL);
    TEST_ASSERT_EQUAL_size_t(5, HA.n);

    /* a light: its brightness, and that it can be dimmed; not a setpoint or modes */
    const dev_values_t *v = &HA.devices[0].values;
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_LEVEL | DEV_VAL_LEVEL_SET, v->flags);
    TEST_ASSERT_EQUAL_UINT8(40, v->level);
    TEST_ASSERT_EQUAL_HEX8(0, v->modes);

    /* a thermostat: its setpoint, temperature and the modes the screen knows; no level */
    v = &HA.devices[1].values;
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_SETPOINT | DEV_VAL_TEMP, v->flags);
    TEST_ASSERT_EQUAL_INT32(2150, v->setpoint);
    TEST_ASSERT_EQUAL_INT32(1975, v->temp);
    TEST_ASSERT_EQUAL_HEX8(DEV_MODE_OFF | DEV_MODE_HEAT | DEV_MODE_HEAT_COOL, v->modes);

    /* a fan: its speed, but no slider (its sheet has speed keys) */
    v = &HA.devices[2].values;
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_LEVEL, v->flags);
    TEST_ASSERT_EQUAL_UINT8(67, v->level);

    /* a lock shows none of them; a cover sent without values knows nothing */
    TEST_ASSERT_EQUAL_HEX8(0, HA.devices[3].values.flags);
    TEST_ASSERT_EQUAL_HEX8(0, HA.devices[3].values.modes);
    TEST_ASSERT_EQUAL_HEX8(0, HA.devices[4].values.flags);

    /* out of range is not known: a level past 100, a temperature past DEV_TEMP_MAX */
    const dev_values_t wild = values(DEV_VAL_LEVEL, 101, 0, 0, 0);
    dsrc_ha_catalog_add(&HA, "light.porch", "Porch", "Outside", "light", "on", &wild);
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_LEVEL_SET, HA.devices[0].values.flags); /* still dimmable */
    dsrc_ha_catalog_add(&HA, "light.hall", "Hall", "Hall", "light", "on", &wild);
    TEST_ASSERT_EQUAL_HEX8(0, HA.devices[5].values.flags); /* new: nothing known */
    const dev_values_t hot = values(DEV_VAL_SETPOINT | DEV_VAL_TEMP, 0, 0, DEV_TEMP_MAX + 1,
                                    -DEV_TEMP_MAX - 1);
    dsrc_ha_catalog_add(&HA, "climate.hall", "Hall", "Hall", "climate", "heat", &hot);
    TEST_ASSERT_EQUAL_HEX8(0, HA.devices[1].values.flags);
    TEST_ASSERT_EQUAL_HEX8(0, HA.devices[1].values.modes);
}

/* HCTL-002 / HCTL-004: a state push replaces the values (what it leaves out is not known now,
 * e.g. a light switched off has no brightness), yet a light that was dimmable keeps its slider;
 * unavailable clears them; an attribute-only change still reaches the screen. */
static void test_ha_values_from_state_pushes(void)
{
    dsrc_ha_link(&HA, true);
    dsrc_ha_catalog_add(&HA, "light.porch", "Porch", "Outside", "light", "off", NULL);
    dsrc_ha_catalog_add(&HA, "climate.hall", "Hall", "Hall", "climate", "heat", NULL);
    dsrc_ha_catalog_add(&HA, "cover.garage", "Garage", "Garage", "cover", "open", NULL);
    TEST_ASSERT_EQUAL_HEX8(0, HA.devices[0].values.flags); /* off at the start: not known yet */

    const dev_values_t half = values(DEV_VAL_LEVEL, 50, 0, 0, 0);
    dsrc_ha_state(&HA, "light.porch", "on", true, &half);
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_LEVEL | DEV_VAL_LEVEL_SET, HA.devices[0].values.flags);
    TEST_ASSERT_EQUAL_UINT8(50, HA.devices[0].values.level);

    /* only the brightness moved: the screen hears of it */
    const unsigned changes = CHANGES;
    const dev_values_t bright = values(DEV_VAL_LEVEL, 80, 0, 0, 0);
    dsrc_ha_state(&HA, "light.porch", "on", true, &bright);
    TEST_ASSERT_EQUAL_UINT8(80, HA.devices[0].values.level);
    TEST_ASSERT_TRUE(CHANGES > changes);

    /* off: no brightness any more, still dimmable */
    dsrc_ha_state(&HA, "light.porch", "off", true, NULL);
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_LEVEL_SET, HA.devices[0].values.flags);

    /* a thermostat's setpoint goes when Home Assistant has a range instead (heat_cool) */
    const dev_values_t heat = values(DEV_VAL_SETPOINT | DEV_VAL_TEMP, 0, DEV_MODE_HEAT, 2100, 1900);
    dsrc_ha_state(&HA, "climate.hall", "heat", true, &heat);
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_SETPOINT | DEV_VAL_TEMP, HA.devices[1].values.flags);
    const dev_values_t range = values(DEV_VAL_TEMP, 0, DEV_MODE_HEAT_COOL, 0, 1900);
    dsrc_ha_state(&HA, "climate.hall", "heat_cool", true, &range);
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_TEMP, HA.devices[1].values.flags);
    TEST_ASSERT_EQUAL_HEX8(DEV_MODE_HEAT_COOL, HA.devices[1].values.modes);

    /* a cover's position */
    const dev_values_t pos = values(DEV_VAL_LEVEL, 30, 0, 0, 0);
    dsrc_ha_state(&HA, "cover.garage", "open", true, &pos);
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_LEVEL | DEV_VAL_LEVEL_SET, HA.devices[2].values.flags);
    TEST_ASSERT_EQUAL_UINT8(30, HA.devices[2].values.level);

    /* unavailable over there: nothing stale is left, values included */
    dsrc_ha_state(&HA, "climate.hall", "heat", false, &heat);
    TEST_ASSERT_EQUAL_HEX8(0, HA.devices[1].values.flags);
    TEST_ASSERT_EQUAL_HEX8(0, HA.devices[1].values.modes);

    /* and the registry hands the values on as they are */
    TEST_ASSERT_TRUE(hc_register(&HC, dsrc_ha_ops(), &HA));
    hc_refresh(&HC);
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_LEVEL | DEV_VAL_LEVEL_SET,
                           hc_get(&HC, "cover.garage")->info.values.flags);
    TEST_ASSERT_EQUAL_UINT8(30, hc_get(&HC, "cover.garage")->info.values.level);
}

/* HCTL-002 / HCTL-004 (BL-171 review): a light that has reported a brightness keeps its slider
 * through a new catalog (a reconnect, a resend) even while it is off and so sends no level; a
 * light never seen dimmed gets none, and the panel's own restart (dsrc_ha_init) forgets. */
static void test_ha_dimmable_survives_a_new_catalog(void)
{
    dsrc_ha_link(&HA, true);
    dsrc_ha_catalog_add(&HA, "light.porch", "Porch", "Outside", "light", "off", NULL);
    dsrc_ha_catalog_add(&HA, "light.hall", "Hall", "Hall", "light", "off", NULL);
    dsrc_ha_catalog_add(&HA, "switch.pump", "Pump", "Garden", "switch", "off", NULL);
    const dev_values_t half = values(DEV_VAL_LEVEL, 50, 0, 0, 0);
    dsrc_ha_state(&HA, "light.porch", "on", true, &half); /* dimmed once */
    dsrc_ha_state(&HA, "light.porch", "off", true, NULL);
    const dev_values_t pos = values(DEV_VAL_LEVEL, 30, 0, 0, 0);
    dsrc_ha_catalog_add(&HA, "cover.garage", "Garage", "Garage", "cover", "open", &pos);

    /* the link drops and comes back: a fresh catalog, every light off, no levels in it */
    dsrc_ha_link(&HA, false);
    dsrc_ha_link(&HA, true);
    dsrc_ha_catalog_begin(&HA);
    TEST_ASSERT_EQUAL_size_t(0, HA.n);
    dsrc_ha_catalog_add(&HA, "light.hall", "Hall", "Hall", "light", "off", NULL);
    dsrc_ha_catalog_add(&HA, "light.porch", "Porch", "Outside", "light", "off", NULL);
    dsrc_ha_catalog_add(&HA, "cover.garage", "Garage", "Garage", "cover", "closed", NULL);
    dsrc_ha_catalog_add(&HA, "switch.pump", "Pump", "Garden", "switch", "off", NULL);
    TEST_ASSERT_EQUAL_STRING("light.porch", HA.devices[1].ref);
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_LEVEL_SET, HA.devices[1].values.flags); /* still dimmable */
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_LEVEL_SET, HA.devices[2].values.flags); /* the cover too */
    TEST_ASSERT_EQUAL_HEX8(0, HA.devices[0].values.flags); /* never dimmed: no slider */
    TEST_ASSERT_EQUAL_HEX8(0, HA.devices[3].values.flags); /* a switch never has one */

    /* a state push without a level after the catalog keeps it as well */
    dsrc_ha_state(&HA, "light.porch", "off", true, NULL);
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_LEVEL_SET, HA.devices[1].values.flags);

    /* the remembered list is bounded: more dimmed lights than rows replace the oldest */
    char ref[24];
    for (unsigned i = 0; i < DSRC_HA_MAX + 3u; i++) {
        snprintf(ref, sizeof(ref), "light.l%u", i);
        dsrc_ha_catalog_begin(&HA);
        dsrc_ha_catalog_add(&HA, ref, "L", "A", "light", "on", &half);
    }
    TEST_ASSERT_EQUAL_UINT8(DSRC_HA_MAX, HA.n_dimmable);
    dsrc_ha_catalog_begin(&HA);
    dsrc_ha_catalog_add(&HA, "light.l1", "L", "A", "light", "off", NULL); /* replaced */
    dsrc_ha_catalog_add(&HA, "light.l3", "L", "A", "light", "off", NULL); /* the oldest kept */
    TEST_ASSERT_EQUAL_HEX8(0, HA.devices[0].values.flags);
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_LEVEL_SET, HA.devices[1].values.flags);

    /* the panel restarting starts with nothing remembered */
    dsrc_ha_init(&HA, NULL);
    dsrc_ha_link(&HA, true);
    dsrc_ha_catalog_add(&HA, "light.l3", "L", "A", "light", "off", NULL);
    TEST_ASSERT_EQUAL_HEX8(0, HA.devices[0].values.flags);
}

/* HCTL-004: the screen's Auto key sends "auto"; a thermostat that has only "heat_cool" gets that
 * word instead, one that has "auto" (or both, or none known) keeps "auto"; other modes pass. */
static void test_ha_auto_mode_word(void)
{
    dsrc_ha_link(&HA, true);
    const dev_values_t hc_only = values(0, 0, DEV_MODE_OFF | DEV_MODE_HEAT_COOL, 0, 0);
    const dev_values_t both = values(0, 0, DEV_MODE_AUTO | DEV_MODE_HEAT_COOL, 0, 0);
    dsrc_ha_catalog_add(&HA, "climate.hall", "Hall", "Hall", "climate", "off", &hc_only);
    dsrc_ha_catalog_add(&HA, "climate.den", "Den", "Den", "climate", "off", &both);
    dsrc_ha_catalog_add(&HA, "climate.attic", "Attic", "Attic", "climate", "off", NULL);
    const device_source_ops_t *ops = dsrc_ha_ops();

    TEST_ASSERT_TRUE(ops->act(&HA, "climate.hall", ACT_SET_MODE, 0, "auto", 1));
    TEST_ASSERT_EQUAL_STRING("heat_cool", SEND.mode);
    TEST_ASSERT_TRUE(ops->act(&HA, "climate.hall", ACT_SET_MODE, 0, "off", 2));
    TEST_ASSERT_EQUAL_STRING("off", SEND.mode);
    TEST_ASSERT_TRUE(ops->act(&HA, "climate.den", ACT_SET_MODE, 0, "auto", 3));
    TEST_ASSERT_EQUAL_STRING("auto", SEND.mode);
    TEST_ASSERT_TRUE(ops->act(&HA, "climate.attic", ACT_SET_MODE, 0, "auto", 4));
    TEST_ASSERT_EQUAL_STRING("auto", SEND.mode);
}

/* HCTL-006 (BL-171): the row names the action under way or failed whoever started it - the
 * console's hc_act or an automation's hc_act_auto - so the screen says "Didn't turn on", not
 * "Didn't respond"; a success clears it, a send that never left names nothing, and a refresh
 * or a timeout keeps it. */
static void test_failed_action_is_kept_for_any_caller(void)
{
    with_two_fake_devices();
    TEST_ASSERT_EQUAL_UINT8(ACT_NONE, hc_get(&HC, "zb/porch")->action);

    /* an automation turns the light on and Home Assistant says no */
    uint32_t id = 0;
    TEST_ASSERT_EQUAL(HC_OK, hc_act_auto(&HC, "zb/porch", ACT_ON, -1, NULL, false, &id));
    TEST_ASSERT_EQUAL_UINT8(ACT_ON, hc_get(&HC, "zb/porch")->action); /* under way */
    hc_sink(&HC)->result(&HC, id, false, "failed", false);
    const hc_device_t *d = hc_get(&HC, "zb/porch");
    TEST_ASSERT_TRUE(d->failed);
    TEST_ASSERT_EQUAL_UINT8(ACT_ON, d->action);

    /* a refresh (a state push) keeps it named */
    hc_refresh(&HC);
    TEST_ASSERT_EQUAL_UINT8(ACT_ON, hc_get(&HC, "zb/porch")->action);

    /* the console's lock times out: named too */
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/front", ACT_LOCK, 0, NULL));
    hc_tick(&HC, 1);
    hc_tick(&HC, 1u + HC_ACT_TIMEOUT_MS);
    d = hc_get(&HC, "zb/front");
    TEST_ASSERT_TRUE(d->failed);
    TEST_ASSERT_EQUAL_STRING("timeout", d->error);
    TEST_ASSERT_EQUAL_UINT8(ACT_LOCK, d->action);

    /* the next try succeeds: nothing left to say */
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/porch", ACT_OFF, 0, NULL));
    TEST_ASSERT_EQUAL_UINT8(ACT_OFF, hc_get(&HC, "zb/porch")->action);
    hc_sink(&HC)->result(&HC, FAKE.act_id, true, "", false);
    TEST_ASSERT_FALSE(hc_get(&HC, "zb/porch")->failed);
    TEST_ASSERT_EQUAL_UINT8(ACT_NONE, hc_get(&HC, "zb/porch")->action);

    /* the source refuses at once: nothing went out, nothing is named */
    FAKE.act_returns = false;
    TEST_ASSERT_EQUAL(HC_ERR_OFFLINE, hc_act(&HC, "zb/porch", ACT_ON, -1, NULL));
    TEST_ASSERT_EQUAL_UINT8(ACT_NONE, hc_get(&HC, "zb/porch")->action);
    FAKE.act_returns = true;

    /* the connection drops under an action: it fails as "offline", still named */
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/porch", ACT_TOGGLE, 0, NULL));
    FAKE.online = false;
    hc_refresh(&HC);
    FAKE.online = true;
    hc_refresh(&HC);
    d = hc_get(&HC, "zb/porch");
    TEST_ASSERT_TRUE(d->failed);
    TEST_ASSERT_EQUAL_STRING("offline", d->error);
    TEST_ASSERT_EQUAL_UINT8(ACT_TOGGLE, d->action);
}

/* ---- a newer report clears a failed action's words (BL-175) ---------------------------------- */

/* The fake source's device by ref, to change what it reports. */
static dev_info_t *fake_dev(const char *ref)
{
    for (size_t i = 0; i < FAKE.n; i++) {
        if (strcmp(FAKE.devs[i].ref, ref) == 0) {
            return &FAKE.devs[i];
        }
    }
    TEST_FAIL_MESSAGE("no such fake device");
    return NULL;
}

/* Sends an action to ref and has its source refuse it. */
static void fail_action(const char *ref, dev_action_t action, int32_t value)
{
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, ref, action, value, NULL));
    hc_sink(&HC)->result(&HC, FAKE.act_id, false, "failed", false);
    const hc_device_t *d = hc_get(&HC, ref);
    TEST_ASSERT_TRUE(d->failed);
    TEST_ASSERT_EQUAL_UINT8(action, d->action);
}

static void assert_failed(const char *ref, dev_action_t action, const char *error)
{
    const hc_device_t *d = hc_get(&HC, ref);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_TRUE(d->failed);
    TEST_ASSERT_EQUAL_UINT8(action, d->action);
    TEST_ASSERT_EQUAL_STRING(error, d->error);
}

static void assert_cleared(const char *ref)
{
    const hc_device_t *d = hc_get(&HC, ref);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_FALSE(d->failed);
    TEST_ASSERT_EQUAL_UINT8(ACT_NONE, d->action);
    TEST_ASSERT_EQUAL_STRING("", d->error);
    TEST_ASSERT_EQUAL_UINT32(0, d->pending);
}

/* HCTL-006, BL-175: a failed action's words stay while the source reports what the device showed
 * when it failed, and go once it reports another state word - the state speaks then, as Home
 * Assistant's cards show the state apart from a failed call. The result isn't reported again. */
static void test_failure_cleared_by_a_newer_state(void)
{
    fake_add(&FAKE, "zb/fan", DEV_FAN, "Fan", "Hall", "off", true);
    TEST_ASSERT_TRUE(hc_register(&HC, &FAKE_OPS, &FAKE));
    hc_refresh(&HC);

    fail_action("zb/fan", ACT_SET_LEVEL, 50);
    TEST_ASSERT_EQUAL_UINT(1, RESULTS);

    hc_refresh(&HC); /* the same "off" again */
    assert_failed("zb/fan", ACT_SET_LEVEL, "failed");
    hc_refresh(&HC);
    assert_failed("zb/fan", ACT_SET_LEVEL, "failed");

    snprintf(fake_dev("zb/fan")->state, DEV_STATE_LEN, "%s", "on");
    hc_refresh(&HC);
    assert_cleared("zb/fan");
    TEST_ASSERT_EQUAL_STRING("on", hc_get(&HC, "zb/fan")->info.state);
    TEST_ASSERT_EQUAL_UINT(1, RESULTS); /* the clear is no new result */

    /* it stays cleared: the next refresh has nothing to bring back */
    hc_refresh(&HC);
    assert_cleared("zb/fan");
}

/* HCTL-006, BL-175: a newer level or setpoint clears it too, with the state word unchanged. */
static void test_failure_cleared_by_a_newer_value(void)
{
    fake_add(&FAKE, "zb/lamp", DEV_LIGHT, "Lamp", "Hall", "on", true);
    fake_add(&FAKE, "zb/stat", DEV_CLIMATE, "Hallway", "Hall", "heat", true);
    fake_dev("zb/lamp")->values = values(DEV_VAL_LEVEL | DEV_VAL_LEVEL_SET, 40, 0, 0, 0);
    fake_dev("zb/stat")->values =
        values(DEV_VAL_SETPOINT | DEV_VAL_TEMP, 0, DEV_MODE_HEAT | DEV_MODE_OFF, 2100, 2000);
    TEST_ASSERT_TRUE(hc_register(&HC, &FAKE_OPS, &FAKE));
    hc_refresh(&HC);

    fail_action("zb/lamp", ACT_SET_LEVEL, 80);
    hc_refresh(&HC);
    assert_failed("zb/lamp", ACT_SET_LEVEL, "failed");
    fake_dev("zb/lamp")->values.level = 80; /* "on" still */
    hc_refresh(&HC);
    assert_cleared("zb/lamp");

    fail_action("zb/stat", ACT_SET_TEMP, 2200);
    hc_refresh(&HC);
    assert_failed("zb/stat", ACT_SET_TEMP, "failed");
    fake_dev("zb/stat")->values.setpoint = 2200; /* "heat" still */
    hc_refresh(&HC);
    assert_cleared("zb/stat");

    /* a level that becomes known is newer too: it says something the failure didn't see */
    fake_dev("zb/lamp")->values.flags = 0;
    hc_refresh(&HC);
    fail_action("zb/lamp", ACT_SET_LEVEL, 30);
    hc_refresh(&HC);
    assert_failed("zb/lamp", ACT_SET_LEVEL, "failed");
    fake_dev("zb/lamp")->values.flags = DEV_VAL_LEVEL;
    hc_refresh(&HC);
    assert_cleared("zb/lamp");
}

/* HCTL-006, BL-175: what moves by itself or says what a device can do isn't newer - a
 * temperature, what a thermostat is doing, its modes, its capabilities, a non-scene's since, a
 * name - and neither is another device's change: the failure stays. */
static void test_failure_kept_on_the_same_report(void)
{
    fake_add(&FAKE, "zb/stat", DEV_CLIMATE, "Hallway", "Hall", "heat", true);
    fake_add(&FAKE, "zb/porch", DEV_LIGHT, "Porch", "Outside", "on", true);
    fake_dev("zb/stat")->values =
        values(DEV_VAL_SETPOINT | DEV_VAL_TEMP, 0, DEV_MODE_HEAT | DEV_MODE_OFF, 2100, 2000);
    TEST_ASSERT_TRUE(hc_register(&HC, &FAKE_OPS, &FAKE));
    hc_refresh(&HC);

    fail_action("zb/stat", ACT_SET_MODE, 0);
    dev_info_t *s = fake_dev("zb/stat");

    hc_refresh(&HC); /* the same report */
    assert_failed("zb/stat", ACT_SET_MODE, "failed");
    s->values.temp = 2050; /* the room warms up */
    hc_refresh(&HC);
    assert_failed("zb/stat", ACT_SET_MODE, "failed");
    s->values.flags = DEV_VAL_SETPOINT; /* the temperature goes unknown */
    hc_refresh(&HC);
    assert_failed("zb/stat", ACT_SET_MODE, "failed");
    s->values.hvac_action = (uint8_t)DEV_HVAC_HEATING;
    hc_refresh(&HC);
    assert_failed("zb/stat", ACT_SET_MODE, "failed");
    s->values.modes = DEV_MODE_HEAT | DEV_MODE_OFF | DEV_MODE_COOL;
    hc_refresh(&HC);
    assert_failed("zb/stat", ACT_SET_MODE, "failed");
    s->values.caps = DEV_CAP_KNOWN;
    hc_refresh(&HC);
    assert_failed("zb/stat", ACT_SET_MODE, "failed");
    s->values.since = 1700000000u; /* only a scene's run time counts */
    hc_refresh(&HC);
    assert_failed("zb/stat", ACT_SET_MODE, "failed");
    snprintf(s->name, DEV_NAME_LEN, "%s", "Upstairs");
    hc_refresh(&HC);
    assert_failed("zb/stat", ACT_SET_MODE, "failed");
    snprintf(fake_dev("zb/porch")->state, DEV_STATE_LEN, "%s", "off"); /* another device */
    hc_refresh(&HC);
    assert_failed("zb/stat", ACT_SET_MODE, "failed");
    TEST_ASSERT_EQUAL_STRING("off", hc_get(&HC, "zb/porch")->info.state);

    /* its own state word is newer */
    snprintf(s->state, DEV_STATE_LEN, "%s", "off");
    hc_refresh(&HC);
    assert_cleared("zb/stat");
}

/* HCTL-006, BL-175: while the device is offline nothing is compared; back online with what it
 * showed, the failure stays; with something else, it goes. */
static void test_failure_kept_while_offline(void)
{
    fake_add(&FAKE, "zb/front", DEV_LOCK, "Front Door", "Hall", "locked", true);
    TEST_ASSERT_TRUE(hc_register(&HC, &FAKE_OPS, &FAKE));
    hc_refresh(&HC);
    fail_action("zb/front", ACT_UNLOCK, 0);

    dev_info_t *f = fake_dev("zb/front");
    f->online = false;
    f->state[0] = '\0'; /* unavailable: no state word */
    hc_refresh(&HC);
    assert_failed("zb/front", ACT_UNLOCK, "failed");
    TEST_ASSERT_FALSE(hc_get(&HC, "zb/front")->info.online);
    snprintf(f->state, DEV_STATE_LEN, "%s", "unlocked"); /* still offline: not compared */
    hc_refresh(&HC);
    assert_failed("zb/front", ACT_UNLOCK, "failed");

    f->online = true;
    snprintf(f->state, DEV_STATE_LEN, "%s", "locked"); /* back with what it had */
    hc_refresh(&HC);
    assert_failed("zb/front", ACT_UNLOCK, "failed");

    snprintf(f->state, DEV_STATE_LEN, "%s", "unlocked");
    hc_refresh(&HC);
    assert_cleared("zb/front");
}

/* HCTL-012, BL-175: a state never touches an action under way; when it then fails, what the
 * device showed is the newest report, so the same report keeps the failure and another clears it. */
static void test_pending_unaffected_by_states(void)
{
    fake_add(&FAKE, "zb/porch", DEV_LIGHT, "Porch", "Outside", "off", true);
    TEST_ASSERT_TRUE(hc_register(&HC, &FAKE_OPS, &FAKE));
    hc_refresh(&HC);

    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/porch", ACT_ON, -1, NULL));
    const uint32_t id = FAKE.act_id;
    dev_info_t *p = fake_dev("zb/porch");
    snprintf(p->state, DEV_STATE_LEN, "%s", "on");
    hc_refresh(&HC);
    snprintf(p->state, DEV_STATE_LEN, "%s", "off");
    p->values = values(DEV_VAL_LEVEL, 10, 0, 0, 0);
    hc_refresh(&HC);
    const hc_device_t *d = hc_get(&HC, "zb/porch");
    TEST_ASSERT_EQUAL_UINT32(id, d->pending);
    TEST_ASSERT_FALSE(d->failed);
    TEST_ASSERT_EQUAL_UINT8(ACT_ON, d->action);

    hc_sink(&HC)->result(&HC, id, false, "failed", false);
    assert_failed("zb/porch", ACT_ON, "failed");
    hc_refresh(&HC); /* "off" at 10, as when it failed */
    assert_failed("zb/porch", ACT_ON, "failed");
    snprintf(p->state, DEV_STATE_LEN, "%s", "on");
    hc_refresh(&HC);
    assert_cleared("zb/porch");
}

/* HCTL-012, BL-175: a timeout's failure and a dropped connection's ("offline") are kept on the
 * same report - a reconnect resending every state included - and go once the device is online
 * and reports another state. */
static void test_timeout_and_offline_failures_clear_on_a_newer_state(void)
{
    with_two_fake_devices(); /* porch "on", front "locked" */

    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/front", ACT_UNLOCK, 0, NULL));
    hc_tick(&HC, 1);
    hc_tick(&HC, 1u + HC_ACT_TIMEOUT_MS);
    assert_failed("zb/front", ACT_UNLOCK, "timeout");
    hc_refresh(&HC);
    assert_failed("zb/front", ACT_UNLOCK, "timeout");
    snprintf(fake_dev("zb/front")->state, DEV_STATE_LEN, "%s", "unlocked");
    hc_refresh(&HC);
    assert_cleared("zb/front");

    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/porch", ACT_OFF, 0, NULL));
    FAKE.online = false;
    hc_refresh(&HC);
    FAKE.online = true;
    hc_refresh(&HC); /* the reconnect: the same "on" */
    assert_failed("zb/porch", ACT_OFF, "offline");
    hc_refresh(&HC);
    assert_failed("zb/porch", ACT_OFF, "offline");
    snprintf(fake_dev("zb/porch")->state, DEV_STATE_LEN, "%s", "off");
    hc_refresh(&HC);
    assert_cleared("zb/porch");
}

/* HCTL-006, BL-175: a scene's failed run, through Home Assistant: kept on the same run time and
 * across a link drop and reconnect (the panel's drop time is never compared), cleared once the
 * scene reports that it ran again. */
static void test_scene_failure_cleared_when_it_runs(void)
{
    dev_values_t v = values(0, 0, 0, 0, 0);
    v.since = 1700000000u;
    dsrc_ha_link(&HA, true);
    dsrc_ha_catalog_begin(&HA);
    dsrc_ha_catalog_add(&HA, "scene.evening", "Evening", "", "scene", "", &v);
    TEST_ASSERT_TRUE(hc_register(&HC, dsrc_ha_ops(), &HA));
    hc_refresh(&HC);

    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "scene.evening", ACT_RUN, 0, NULL));
    TEST_ASSERT_EQUAL_UINT(1, SEND.calls);
    hc_sink(&HC)->result(&HC, SEND.request_id, false, "failed", false);
    assert_failed("scene.evening", ACT_RUN, "failed");

    dsrc_ha_state(&HA, "scene.evening", "", true, &v); /* the same run time */
    hc_refresh(&HC);
    assert_failed("scene.evening", ACT_RUN, "failed");

    dsrc_ha_link_at(&HA, false, 1700000600u);
    hc_refresh(&HC);
    TEST_ASSERT_FALSE(hc_get(&HC, "scene.evening")->info.online);
    assert_failed("scene.evening", ACT_RUN, "failed");
    dsrc_ha_link_at(&HA, true, 0);
    hc_refresh(&HC);
    TEST_ASSERT_TRUE(hc_get(&HC, "scene.evening")->info.online);
    TEST_ASSERT_EQUAL_UINT32(1700000000u, hc_get(&HC, "scene.evening")->info.values.since);
    assert_failed("scene.evening", ACT_RUN, "failed");

    v.since = 1700000900u; /* it ran */
    dsrc_ha_state(&HA, "scene.evening", "", true, &v);
    hc_refresh(&HC);
    assert_cleared("scene.evening");
}

/* HCTL-012 (BL-171): whether a failed action's outcome is unknown comes from where the failure
 * came from, never from the registry reading its word. A source's refusal made before anything
 * was sent is definite ("didn't"), whatever it says; a source's answer that it sent the action
 * on and couldn't confirm it (the sink's unknown) and the registry's own failures of an action
 * in flight - no answer by the deadline, its connection dropped under it - are unknown
 * ("Couldn't confirm"), kept across refreshes while the failure is, and gone with the next act, a
 * success or a newer report (BL-175). */
static void test_outcome_unknown_by_origin(void)
{
    TEST_ASSERT_FALSE(hc_outcome_unknown(NULL));
    with_two_fake_devices(); /* porch "on", front "locked" */
    TEST_ASSERT_FALSE(hc_outcome_unknown(hc_get(&HC, "zb/porch"))); /* nothing failed */

    /* the source's refusals, every word: definite - the word itself decides nothing */
    static const char *const SOURCE_WORDS[] = {
        "failed", "not_allowed", "unavailable", "busy", "offline", "timeout", "send_failed",
        "something_new",
    };
    for (size_t i = 0; i < sizeof SOURCE_WORDS / sizeof SOURCE_WORDS[0]; i++) {
        TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/front", ACT_UNLOCK, 0, NULL));
        TEST_ASSERT_FALSE(hc_outcome_unknown(hc_get(&HC, "zb/front"))); /* under way */
        hc_sink(&HC)->result(&HC, FAKE.act_id, false, SOURCE_WORDS[i], false);
        const hc_device_t *d = hc_get(&HC, "zb/front");
        TEST_ASSERT_TRUE(d->failed);
        TEST_ASSERT_EQUAL_STRING(SOURCE_WORDS[i], d->error);
        TEST_ASSERT_FALSE_MESSAGE(hc_outcome_unknown(d), SOURCE_WORDS[i]);
    }
    /* ... and the same words, from a source that says it sent them on unconfirmed: unknown */
    for (size_t i = 0; i < sizeof SOURCE_WORDS / sizeof SOURCE_WORDS[0]; i++) {
        TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/front", ACT_UNLOCK, 0, NULL));
        hc_sink(&HC)->result(&HC, FAKE.act_id, false, SOURCE_WORDS[i], true);
        const hc_device_t *d = hc_get(&HC, "zb/front");
        TEST_ASSERT_TRUE(d->failed);
        TEST_ASSERT_EQUAL_STRING(SOURCE_WORDS[i], d->error);
        TEST_ASSERT_TRUE_MESSAGE(hc_outcome_unknown(d), SOURCE_WORDS[i]);
        hc_refresh(&HC); /* kept on the same report */
        TEST_ASSERT_TRUE_MESSAGE(hc_outcome_unknown(hc_get(&HC, "zb/front")), SOURCE_WORDS[i]);
    }
    /* a success never is, whatever the source says */
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/front", ACT_UNLOCK, 0, NULL));
    hc_sink(&HC)->result(&HC, FAKE.act_id, true, "", true);
    TEST_ASSERT_FALSE(hc_get(&HC, "zb/front")->failed);
    TEST_ASSERT_FALSE(hc_get(&HC, "zb/front")->unsure);
    /* no word at all (stored as "failed"), and NULL: definite */
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/front", ACT_UNLOCK, 0, NULL));
    hc_sink(&HC)->result(&HC, FAKE.act_id, false, "", false);
    TEST_ASSERT_EQUAL_STRING("failed", hc_get(&HC, "zb/front")->error);
    TEST_ASSERT_FALSE(hc_outcome_unknown(hc_get(&HC, "zb/front")));
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/front", ACT_UNLOCK, 0, NULL));
    hc_sink(&HC)->result(&HC, FAKE.act_id, false, NULL, false);
    TEST_ASSERT_FALSE(hc_outcome_unknown(hc_get(&HC, "zb/front")));

    /* no answer by the deadline: unknown, kept on the same report, cleared by the next act */
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/front", ACT_UNLOCK, 0, NULL));
    hc_tick(&HC, 1);
    hc_tick(&HC, 1u + HC_ACT_TIMEOUT_MS);
    assert_failed("zb/front", ACT_UNLOCK, "timeout");
    TEST_ASSERT_TRUE(hc_outcome_unknown(hc_get(&HC, "zb/front")));
    hc_refresh(&HC);
    TEST_ASSERT_TRUE(hc_outcome_unknown(hc_get(&HC, "zb/front")));
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/front", ACT_UNLOCK, 0, NULL));
    TEST_ASSERT_FALSE(hc_outcome_unknown(hc_get(&HC, "zb/front")));
    /* ... and a later refusal of that retry is definite again */
    hc_sink(&HC)->result(&HC, FAKE.act_id, false, "unavailable", false);
    TEST_ASSERT_FALSE(hc_outcome_unknown(hc_get(&HC, "zb/front")));

    /* a timeout whose device then reports something newer: the failure goes, so does unknown */
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/front", ACT_UNLOCK, 0, NULL));
    hc_tick(&HC, 2u * HC_ACT_TIMEOUT_MS);
    hc_tick(&HC, 3u * HC_ACT_TIMEOUT_MS);
    TEST_ASSERT_TRUE(hc_outcome_unknown(hc_get(&HC, "zb/front")));
    snprintf(fake_dev("zb/front")->state, DEV_STATE_LEN, "%s", "unlocked");
    hc_refresh(&HC);
    assert_cleared("zb/front");
    TEST_ASSERT_FALSE(hc_get(&HC, "zb/front")->unsure);
    TEST_ASSERT_FALSE(hc_outcome_unknown(hc_get(&HC, "zb/front")));

    /* the connection drops with an action in flight: "offline", unknown - unlike the source's
     * own "offline" above - and still unknown after the reconnect's same report */
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/porch", ACT_OFF, 0, NULL));
    FAKE.online = false;
    hc_refresh(&HC);
    assert_failed("zb/porch", ACT_OFF, "offline");
    TEST_ASSERT_TRUE(hc_outcome_unknown(hc_get(&HC, "zb/porch")));
    FAKE.online = true;
    hc_refresh(&HC);
    assert_failed("zb/porch", ACT_OFF, "offline");
    TEST_ASSERT_TRUE(hc_outcome_unknown(hc_get(&HC, "zb/porch")));

    /* a success clears it */
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/porch", ACT_OFF, 0, NULL));
    hc_sink(&HC)->result(&HC, FAKE.act_id, true, "", false);
    TEST_ASSERT_FALSE(hc_outcome_unknown(hc_get(&HC, "zb/porch")));
    TEST_ASSERT_FALSE(hc_get(&HC, "zb/porch")->unsure);

    /* a refresh drops the device with its action in flight: its caller hears "offline" and the
     * row is gone (nothing left to call unknown) */
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "zb/porch", ACT_ON, -1, NULL));
    const uint32_t id = FAKE.act_id;
    FAKE.devs[0] = FAKE.devs[1];
    FAKE.n = 1;
    const unsigned results = RESULTS;
    hc_refresh(&HC);
    TEST_ASSERT_EQUAL_UINT(results + 1u, RESULTS);
    TEST_ASSERT_EQUAL_UINT32(id, R_ID);
    TEST_ASSERT_EQUAL_STRING("offline", R_ERR);
    TEST_ASSERT_NULL(hc_get(&HC, "zb/porch"));
    TEST_ASSERT_FALSE(hc_outcome_unknown(hc_get(&HC, "zb/porch")));
}

/* HCTL-012: Home Assistant's call_result "timeout" means the integration started the service
 * call and it didn't finish within its own 25 s - before the registry's 30 s deadline - so the
 * lock may have unlocked and not said so: unknown ("Couldn't confirm"), never "didn't". Its
 * "failed", "not_allowed" and "unavailable" stay definite. The whole way round: hc_act on an HA
 * lock, the call goes out, the answer comes back through dsrc_ha_result as ha_link hands it. */
static void test_ha_timeout_is_an_unknown_outcome(void)
{
    dsrc_ha_link(&HA, true);
    dsrc_ha_catalog_begin(&HA);
    dsrc_ha_catalog_add(&HA, "lock.front", "Front Door", "Hall", "lock", "locked", NULL);
    TEST_ASSERT_TRUE(hc_register(&HC, dsrc_ha_ops(), &HA));
    hc_refresh(&HC);

    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "lock.front", ACT_UNLOCK, 0, NULL));
    TEST_ASSERT_EQUAL_STRING("unlock", SEND.action);
    hc_tick(&HC, 1); /* the registry's deadline starts: HA's timeout comes well before it */
    hc_tick(&HC, 25001);
    TEST_ASSERT_EQUAL_UINT32(SEND.request_id, hc_get(&HC, "lock.front")->pending);
    dsrc_ha_result(&HA, SEND.request_id, false, "timeout");
    TEST_ASSERT_EQUAL_UINT(1, RESULTS);
    TEST_ASSERT_FALSE(R_OK);
    TEST_ASSERT_EQUAL_STRING("timeout", R_ERR);
    assert_failed("lock.front", ACT_UNLOCK, "timeout");
    TEST_ASSERT_TRUE(hc_outcome_unknown(hc_get(&HC, "lock.front")));
    /* the same report (still "locked") keeps it unknown; the registry's own deadline adds nothing */
    dsrc_ha_state(&HA, "lock.front", "locked", true, NULL);
    hc_refresh(&HC);
    hc_tick(&HC, 1u + HC_ACT_TIMEOUT_MS);
    TEST_ASSERT_EQUAL_UINT(1, RESULTS);
    TEST_ASSERT_TRUE(hc_outcome_unknown(hc_get(&HC, "lock.front")));

    static const char *const DEFINITE[] = {"failed", "not_allowed", "unavailable", ""};
    for (size_t i = 0; i < sizeof DEFINITE / sizeof DEFINITE[0]; i++) {
        TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "lock.front", ACT_UNLOCK, 0, NULL));
        dsrc_ha_result(&HA, SEND.request_id, false, DEFINITE[i]);
        const hc_device_t *d = hc_get(&HC, "lock.front");
        TEST_ASSERT_TRUE(d->failed);
        TEST_ASSERT_EQUAL_STRING(DEFINITE[i][0] != '\0' ? DEFINITE[i] : "failed", d->error);
        TEST_ASSERT_FALSE_MESSAGE(hc_outcome_unknown(d), DEFINITE[i]);
    }
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "lock.front", ACT_UNLOCK, 0, NULL));
    dsrc_ha_result(&HA, SEND.request_id, false, NULL);
    TEST_ASSERT_FALSE(hc_outcome_unknown(hc_get(&HC, "lock.front")));

    /* a success after the unknown one clears it */
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "lock.front", ACT_UNLOCK, 0, NULL));
    dsrc_ha_result(&HA, SEND.request_id, false, "timeout");
    TEST_ASSERT_TRUE(hc_outcome_unknown(hc_get(&HC, "lock.front")));
    TEST_ASSERT_EQUAL(HC_OK, hc_act(&HC, "lock.front", ACT_UNLOCK, 0, NULL));
    dsrc_ha_result(&HA, SEND.request_id, true, "");
    TEST_ASSERT_FALSE(hc_get(&HC, "lock.front")->failed);
    TEST_ASSERT_FALSE(hc_outcome_unknown(hc_get(&HC, "lock.front")));
    dsrc_ha_result(&HA, 0u, false, "timeout"); /* no id: nothing */
    TEST_ASSERT_FALSE(hc_get(&HC, "lock.front")->failed);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_register_list_and_get);
    RUN_TEST(test_one_source_per_kind);
    RUN_TEST(test_refresh_merges_and_drops);
    RUN_TEST(test_refresh_skips_unusable_rows);
    RUN_TEST(test_sink_changed_reaches_the_port);
    RUN_TEST(test_needs_code);
    RUN_TEST(test_needs_code_cover_set_level);
    RUN_TEST(test_needs_code_cover_position_below_reported);
    RUN_TEST(test_auto_less_secure_list);
    RUN_TEST(test_auto_allowed_table);
    RUN_TEST(test_auto_offer_table);
    RUN_TEST(test_act_auto_less_secure);
    RUN_TEST(test_act_auto);
    RUN_TEST(test_act_auto_dropped_row_fails_with_its_id);
    RUN_TEST(test_action_valid_table);
    RUN_TEST(test_act_then_result_ok);
    RUN_TEST(test_act_result_failure);
    RUN_TEST(test_act_refusals);
    RUN_TEST(test_act_refused_by_the_source);
    RUN_TEST(test_act_times_out_and_recovers);
    RUN_TEST(test_act_disconnect_invalidates_pending);
    RUN_TEST(test_ha_disconnect_reconnect_recovers);
    RUN_TEST(test_act_duplicate_reply_ignored);
    RUN_TEST(test_act_request_id_wraparound);
    RUN_TEST(test_ha_kind_table);
    RUN_TEST(test_ha_action_words);
    RUN_TEST(test_ha_catalog);
    RUN_TEST(test_ha_state_and_link);
    RUN_TEST(test_ha_act_formats_the_call);
    RUN_TEST(test_ha_list_through_the_registry);
    RUN_TEST(test_end_to_end_through_home_assistant);
    RUN_TEST(test_ha_values_from_the_catalog);
    RUN_TEST(test_ha_values_from_state_pushes);
    RUN_TEST(test_ha_dimmable_survives_a_new_catalog);
    RUN_TEST(test_ha_auto_mode_word);
    RUN_TEST(test_failed_action_is_kept_for_any_caller);
    RUN_TEST(test_failure_cleared_by_a_newer_state);
    RUN_TEST(test_failure_cleared_by_a_newer_value);
    RUN_TEST(test_failure_kept_on_the_same_report);
    RUN_TEST(test_failure_kept_while_offline);
    RUN_TEST(test_pending_unaffected_by_states);
    RUN_TEST(test_timeout_and_offline_failures_clear_on_a_newer_state);
    RUN_TEST(test_scene_failure_cleared_when_it_runs);
    RUN_TEST(test_outcome_unknown_by_origin);
    RUN_TEST(test_ha_timeout_is_an_unknown_outcome);
    return UNITY_END();
}
