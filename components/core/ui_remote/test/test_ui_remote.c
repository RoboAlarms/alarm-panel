/*
 * test_ui_remote.c - the remote backend (docs/features/11, LINK-057..LINK-065; BL-223): a keypad's
 * proxy entries over a pretend channel straight into ui_remote_serve on a fake alarm panel, the
 * grant's rules, and the frozen v1 bytes.
 */
#include <string.h>

#include "alarm/alarm_codec.h"
#include "alarm/alarm_config.h"
#include "ui_remote/ui_remote.h"
#include "ui_remote_fixture_v1.h"
#include "unity.h"
#include "ur_internal.h"

/* The bytes no table field covers on the host build: each struct's padding (see
 * test_tables_cover_the_structs). Set from the first run; a field added to a struct and not to its
 * table raises one. */
#define UR_PAD_BINDING   42u
#define UR_PAD_INFO      50u
#define UR_PAD_CANDIDATE 7u
#define UR_PAD_SOURCE    6u

/* ---- a fake alarm panel: what its backend was asked, and what it answers ---- */
typedef struct {
    alarm_config_t cfg;
    bool program;
    uint8_t program_user;
    int enters, exits, keepalives;
    char last_code[16];
    alarm_result_t user_result;
    char last_actor[16];
    uint8_t last_user;
    uint16_t bound_zone;
    zone_hub_binding_t bound;
    int learn_kind;
    bool learn_on;
    int set_configs;
} fake_t;

static fake_t F;
static ui_remote_server_t S;
static ui_remote_grant_t G;   /* the keypad's grant on the alarm panel */
static ui_remote_grant_t G2;  /* another keypad's */
static ui_remote_grant_t *CUR;
static uint32_t MASTER_CRC = 0x11111111u;
static uint32_t KEYPAD_CRC = 0x11111111u;
static bool SAME_FW = true;
static bool DOWN;             /* the channel says nothing came back */
static uint64_t NOW = 1000;

static void f_snapshot(void *ctx, alarm_snapshot_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    out->program = F.program;
}
static bool f_get_config(void *ctx, alarm_config_t *out)
{
    (void)ctx;
    *out = F.cfg;
    return true;
}
static alarm_result_t f_program_enter(void *ctx, const char *code, uint8_t *user)
{
    (void)ctx;
    F.enters++;
    snprintf(F.last_code, sizeof(F.last_code), "%s", code);
    if (strcmp(code, "69420") != 0) {
        return ALARM_ERR_INVALID_CODE;
    }
    F.program = true;
    *user = 1;
    return ALARM_OK;
}
static void f_program_exit(void *ctx)
{
    (void)ctx;
    F.exits++;
    F.program = false;
}
static void f_keepalive(void *ctx)
{
    (void)ctx;
    F.keepalives++;
}
static alarm_result_t f_set_config(void *ctx, const alarm_config_t *cfg)
{
    (void)ctx;
    F.set_configs++;
    F.cfg = *cfg;
    return ALARM_OK;
}
static alarm_result_t f_user_set(void *ctx, const char *actor, uint8_t user, const char *code,
                                 alarm_authority_t a, uint8_t parts, const char *name)
{
    (void)ctx, (void)code, (void)a, (void)parts, (void)name;
    snprintf(F.last_actor, sizeof(F.last_actor), "%s", actor);
    F.last_user = user;
    return F.user_result;
}
static alarm_result_t f_user_update(void *ctx, const char *actor, uint8_t user,
                                    alarm_authority_t a, uint8_t parts, const char *name)
{
    return f_user_set(ctx, actor, user, "", a, parts, name);
}
static alarm_result_t f_user_delete(void *ctx, const char *actor, uint8_t user)
{
    return f_user_set(ctx, actor, user, "", ALARM_AUTH_NONE, 0, "");
}
static size_t f_sources(void *ctx, ui_zone_source_t *out, size_t max)
{
    (void)ctx;
    if (max < 2u) {
        return 0;
    }
    out[0] = (ui_zone_source_t){.kind = 2, .caps = 0x81u, .usable = true};
    out[1] = (ui_zone_source_t){.kind = 5, .caps = 0x10u, .usable = false};
    return 2;
}
static void f_learn(void *ctx, uint8_t kind, bool on)
{
    (void)ctx;
    F.learn_kind = kind;
    F.learn_on = on;
}
static size_t f_candidates(void *ctx, uint8_t kind, const char *q, zsrc_candidate_t *out, size_t max)
{
    (void)ctx, (void)q;
    size_t n = 0;
    for (; n < 3u && n < max; n++) {
        memset(&out[n], 0, sizeof(out[n]));
        out[n].addr.kind = kind;
        out[n].addr.id = 100u + n;
        snprintf(out[n].name, sizeof(out[n].name), "Sensor %u", (unsigned)n);
        out[n].changed = n == 1u;
    }
    return n;
}
static zone_hub_result_t f_bind(void *ctx, uint16_t zone, const zone_hub_binding_t *b)
{
    (void)ctx;
    F.bound_zone = zone;
    F.bound = *b;
    return ZONE_HUB_OK;
}
static zone_hub_result_t f_unbind(void *ctx, uint16_t zone)
{
    (void)ctx;
    F.bound_zone = zone;
    return ZONE_HUB_OK;
}
static bool f_info(void *ctx, uint16_t zone, zone_hub_info_t *out)
{
    (void)ctx;
    if (zone != F.bound_zone) {
        return false;
    }
    out->binding = F.bound;
    out->source_present = true;
    out->known = true;
    out->active = true;
    out->value_x100 = -2150;
    out->heard_at = 0x0123456789ull;
    snprintf(out->address, sizeof(out->address), "Terminal 4");
    return true;
}

static ui_backend_t MASTER;

static void master_init(void)
{
    memset(&F, 0, sizeof(F));
    alarm_config_defaults(&F.cfg);
    F.cfg.users[0].authority = ALARM_AUTH_INSTALLER;
    F.cfg.users[0].partitions = 1;
    F.cfg.users[1].partitions = 1;
    F.cfg.users[1].authority = ALARM_AUTH_MASTER;
    snprintf(F.cfg.users[1].name, sizeof(F.cfg.users[1].name), "Alex");
    memset(F.cfg.users[1].code_hash, 0x42, sizeof(F.cfg.users[1].code_hash));
    memset(F.cfg.code_salt, 0x24, sizeof(F.cfg.code_salt));
    memset(&MASTER, 0, sizeof(MASTER));
    MASTER.get_snapshot = f_snapshot;
    MASTER.get_config = f_get_config;
    MASTER.program_enter = f_program_enter;
    MASTER.program_exit = f_program_exit;
    MASTER.program_keepalive = f_keepalive;
    MASTER.set_config = f_set_config;
    MASTER.user_set = f_user_set;
    MASTER.user_update = f_user_update;
    MASTER.user_delete = f_user_delete;
    MASTER.zone_sources = f_sources;
    MASTER.zone_learn = f_learn;
    MASTER.zone_candidates = f_candidates;
    MASTER.zone_bind = f_bind;
    MASTER.zone_unbind = f_unbind;
    MASTER.zone_info = f_info;
}

/* ---- the channel: the keypad's call served at once ---- */
static uint8_t ARGS_SEEN[UR_ARGS_MAX];
static size_t ARGS_SEEN_LEN;
static ui_remote_status_t chan_call(void *ctx, uint16_t call, const uint8_t *args, size_t len,
                                    uint8_t *ans, size_t cap, size_t *ans_len)
{
    (void)ctx;
    *ans_len = 0;
    memcpy(ARGS_SEEN, args, len);
    ARGS_SEEN_LEN = len;
    if (DOWN) {
        return UR_NO_ANSWER;
    }
    return ui_remote_serve(&S, &MASTER, CUR, MASTER_CRC, SAME_FW, call, args, len, ans, cap,
                           ans_len, NOW);
}
static uint32_t chan_crc(void *ctx)
{
    (void)ctx;
    return KEYPAD_CRC;
}

static ui_remote_proxy_t PX;
static ui_backend_t KP;
static int KP_INNER;

void setUp(void)
{
    master_init();
    memset(&S, 0, sizeof(S));
    memset(&G, 0, sizeof(G));
    memset(&G2, 0, sizeof(G2));
    CUR = &G;
    MASTER_CRC = KEYPAD_CRC = 0x11111111u;
    SAME_FW = true;
    DOWN = false;
    NOW = 1000;
    memset(&KP, 0, sizeof(KP));
    KP.ctx = &KP_INNER;
    memset(&PX, 0, sizeof(PX));
    PX.port = (ui_remote_port_t){.ctx = NULL, .call = chan_call, .cfg_crc = chan_crc};
    ui_remote_proxy(&KP, &PX, ui_remote_areas(&MASTER));
}

void tearDown(void) {}

/* ---- tests ---- */

/* LINK-058: the call ids, the proxies installed, the old context kept */
static void test_calls_and_proxy(void)
{
    TEST_ASSERT_EQUAL_HEX16(UR_V1_GRANT_END, UR_GRANT_END);
    TEST_ASSERT_EQUAL_HEX16(UR_V1_PROGRAM_ENTER, UR_PROGRAM_ENTER);
    TEST_ASSERT_EQUAL_HEX16(UR_V1_PROGRAM_EXIT, UR_PROGRAM_EXIT);
    TEST_ASSERT_EQUAL_HEX16(UR_V1_PROGRAM_KEEPALIVE, UR_PROGRAM_KEEPALIVE);
    TEST_ASSERT_EQUAL_HEX16(UR_V1_PROGRAM_SET_CODE, UR_PROGRAM_SET_CODE);
    TEST_ASSERT_EQUAL_HEX16(UR_V1_SET_CONFIG, UR_SET_CONFIG);
    TEST_ASSERT_EQUAL_HEX16(UR_V1_USER_SET, UR_USER_SET);
    TEST_ASSERT_EQUAL_HEX16(UR_V1_USER_UPDATE, UR_USER_UPDATE);
    TEST_ASSERT_EQUAL_HEX16(UR_V1_USER_DELETE, UR_USER_DELETE);
    TEST_ASSERT_EQUAL_HEX16(UR_V1_WEB_LOGIN_GET, UR_WEB_LOGIN_GET);
    TEST_ASSERT_EQUAL_HEX16(UR_V1_WEB_LOGIN_SET, UR_WEB_LOGIN_SET);
    TEST_ASSERT_EQUAL_HEX16(UR_V1_WEB_LOGIN_CLEAR, UR_WEB_LOGIN_CLEAR);
    TEST_ASSERT_EQUAL_HEX16(UR_V1_ZONE_SOURCES, UR_ZONE_SOURCES);
    TEST_ASSERT_EQUAL_HEX16(UR_V1_ZONE_LEARN, UR_ZONE_LEARN);
    TEST_ASSERT_EQUAL_HEX16(UR_V1_ZONE_CANDIDATES, UR_ZONE_CANDIDATES);
    TEST_ASSERT_EQUAL_HEX16(UR_V1_ZONE_BIND, UR_ZONE_BIND);
    TEST_ASSERT_EQUAL_HEX16(UR_V1_ZONE_UNBIND, UR_ZONE_UNBIND);
    TEST_ASSERT_EQUAL_HEX16(UR_V1_ZONE_INFO, UR_ZONE_INFO);
    TEST_ASSERT_NULL(ui_remote_info(0x0006));
    TEST_ASSERT_EQUAL_PTR(&PX, KP.ctx);
    TEST_ASSERT_EQUAL_PTR(&KP_INNER, ui_remote_inner(KP.ctx));
    TEST_ASSERT_NOT_NULL(KP.zone_bind);
    TEST_ASSERT_NULL(KP.zw_request); /* not in v1's groups: left as it was */

    /* an unknown call, and a group the alarm panel doesn't serve */
    size_t n = 0;
    uint8_t out[8];
    TEST_ASSERT_EQUAL(UR_UNAVAILABLE,
                      ui_remote_serve(&S, &MASTER, &G, 0, true, 0x0006, NULL, 0, out, sizeof(out), &n, 0));
    MASTER.zone_bind = NULL;
    TEST_ASSERT_EQUAL(UR_UNAVAILABLE,
                      ui_remote_serve(&S, &MASTER, &G, 0, true, UR_ZONE_INFO, NULL, 0, out, sizeof(out), &n, 0));
}

/* LINK-060: the installer code is checked on the alarm panel; installer mode is this keypad's */
static void test_program_enter_from_a_keypad(void)
{
    uint8_t user = 9;
    TEST_ASSERT_EQUAL(ALARM_ERR_INVALID_CODE, KP.program_enter(KP.ctx, "1111", &user));
    TEST_ASSERT_EQUAL_UINT8(0, user);
    TEST_ASSERT_FALSE(G.installer);
    TEST_ASSERT_EQUAL_STRING("1111", F.last_code);
    TEST_ASSERT_EACH_EQUAL_UINT8(0, PX.args, ARGS_SEEN_LEN); /* the code is wiped once sent */

    TEST_ASSERT_EQUAL(ALARM_OK, KP.program_enter(KP.ctx, "69420", &user));
    TEST_ASSERT_EQUAL_UINT8(1, user);
    TEST_ASSERT_TRUE(G.installer);
    TEST_ASSERT_TRUE(F.program);

    KP.program_keepalive(KP.ctx);
    TEST_ASSERT_EQUAL(1, F.keepalives);
    KP.program_exit(KP.ctx);
    TEST_ASSERT_EQUAL(1, F.exits);
    TEST_ASSERT_FALSE(G.installer);
    KP.program_keepalive(KP.ctx); /* no longer this keypad's */
    TEST_ASSERT_EQUAL(1, F.keepalives);
    TEST_ASSERT_EQUAL(UR_NEED_INSTALLER, PX.last);
}

/* LINK-060: one place at a time; ended elsewhere, it is no longer this keypad's */
static void test_installer_mode_one_place(void)
{
    uint8_t user = 0;
    TEST_ASSERT_EQUAL(ALARM_OK, KP.program_enter(KP.ctx, "69420", &user));
    CUR = &G2; /* another keypad */
    TEST_ASSERT_EQUAL(ALARM_ERR_BAD_STATE, KP.program_enter(KP.ctx, "69420", &user));
    TEST_ASSERT_EQUAL(UR_BUSY, PX.last);
    TEST_ASSERT_EQUAL(1, F.enters);
    zone_hub_binding_t b;
    memset(&b, 0, sizeof(b));
    TEST_ASSERT_EQUAL(ZONE_HUB_ERR_BUSY, KP.zone_bind(KP.ctx, 3, &b));
    TEST_ASSERT_EQUAL(UR_BUSY, PX.last);
    TEST_ASSERT_EQUAL_UINT16(0, F.bound_zone);

    CUR = &G;
    F.program = false; /* the alarm panel's own screen ended it */
    TEST_ASSERT_EQUAL(ZONE_HUB_ERR_BUSY, KP.zone_bind(KP.ctx, 3, &b));
    TEST_ASSERT_EQUAL(UR_NEED_INSTALLER, PX.last);
    TEST_ASSERT_FALSE(G.installer);
    TEST_ASSERT_EQUAL(0, F.exits); /* it was already over: not ended again */
}

/* LINK-059: users need a master's code accepted for this keypad; the actor's code goes along */
static void test_users_need_a_grant(void)
{
    TEST_ASSERT_EQUAL(ALARM_ERR_NOT_AUTHORIZED,
                      KP.user_set(KP.ctx, "123456", 3, "2580", ALARM_AUTH_STANDARD, 1, "Pat"));
    TEST_ASSERT_EQUAL(UR_NOT_AUTHORIZED, PX.last);
    TEST_ASSERT_EQUAL_UINT8(0, F.last_user);

    ui_remote_grant_user(&G, 4, ALARM_AUTH_STANDARD, NOW);
    TEST_ASSERT_EQUAL(ALARM_ERR_NOT_AUTHORIZED, KP.user_delete(KP.ctx, "0000", 3));

    ui_remote_grant_user(&G, 2, ALARM_AUTH_MASTER, NOW);
    F.user_result = ALARM_OK;
    TEST_ASSERT_EQUAL(ALARM_OK,
                      KP.user_set(KP.ctx, "123456", 3, "2580", ALARM_AUTH_STANDARD, 1, "Pat"));
    TEST_ASSERT_EQUAL_STRING("123456", F.last_actor);
    TEST_ASSERT_EQUAL_UINT8(3, F.last_user);
    F.user_result = ALARM_ERR_INVALID_CODE; /* the alarm panel judges the actor again */
    TEST_ASSERT_EQUAL(ALARM_ERR_INVALID_CODE, KP.user_update(KP.ctx, "999", 3, ALARM_AUTH_STANDARD, 1, "Pat"));

    /* the grant ends after 5 minutes without a call */
    NOW += UR_GRANT_IDLE_MS;
    ui_remote_grant_tick(&MASTER, &G, false, NOW);
    TEST_ASSERT_EQUAL_UINT8(0, G.user);
    TEST_ASSERT_EQUAL(ALARM_ERR_NOT_AUTHORIZED, KP.user_delete(KP.ctx, "123456", 3));
}

/* LINK-061: a configuration from a keypad: its base must be the alarm panel's, the codes stay */
static void test_set_config_keeps_codes(void)
{
    uint8_t user = 0;
    TEST_ASSERT_EQUAL(ALARM_OK, KP.program_enter(KP.ctx, "69420", &user));
    static alarm_config_t kp;
    kp = F.cfg;
    memset(kp.users[1].code_hash, 0xA5, sizeof(kp.users[1].code_hash)); /* the keypad's filler */
    memset(kp.code_salt, 0, sizeof(kp.code_salt));
    snprintf(kp.users[1].name, sizeof(kp.users[1].name), "Mallory");
    kp.zones[4].type = ALARM_ZT_PERIMETER;
    kp.zones[4].partition = 1;
    snprintf(kp.zones[4].name, sizeof(kp.zones[4].name), "Back Door");
    char why[96] = "";
    TEST_ASSERT_EQUAL_MESSAGE(ALARM_OK, alarm_config_validate(&kp, why, sizeof(why)), why);

    KEYPAD_CRC = 0x22222222u; /* read before a change on the alarm panel */
    TEST_ASSERT_EQUAL(ALARM_ERR_UNAVAILABLE, KP.set_config(KP.ctx, &kp));
    TEST_ASSERT_EQUAL(UR_STALE, PX.last);
    TEST_ASSERT_EQUAL(0, F.set_configs);

    KEYPAD_CRC = MASTER_CRC;
    SAME_FW = false;
    TEST_ASSERT_EQUAL(ALARM_ERR_UNAVAILABLE, KP.set_config(KP.ctx, &kp));
    TEST_ASSERT_EQUAL(0, F.set_configs);

    SAME_FW = true;
    TEST_ASSERT_EQUAL(ALARM_OK, KP.set_config(KP.ctx, &kp));
    TEST_ASSERT_EQUAL(1, F.set_configs);
    TEST_ASSERT_EQUAL_STRING("Back Door", F.cfg.zones[4].name);
    TEST_ASSERT_EQUAL_STRING("Alex", F.cfg.users[1].name);
    TEST_ASSERT_EACH_EQUAL_UINT8(0x42, F.cfg.users[1].code_hash, sizeof(F.cfg.users[1].code_hash));
    TEST_ASSERT_EACH_EQUAL_UINT8(0x24, F.cfg.code_salt, sizeof(F.cfg.code_salt));
}

/* LINK-062: a zone added from a keypad, its sensor learnt on the alarm panel */
static void test_zone_added_from_a_keypad(void)
{
    zone_hub_binding_t b;
    memset(&b, 0, sizeof(b));
    TEST_ASSERT_EQUAL(0u, KP.zone_sources(KP.ctx, NULL, 0)); /* not in installer mode */
    uint8_t user = 0;
    TEST_ASSERT_EQUAL(ALARM_OK, KP.program_enter(KP.ctx, "69420", &user));

    ui_zone_source_t src[4];
    TEST_ASSERT_EQUAL(2u, KP.zone_sources(KP.ctx, src, 4));
    TEST_ASSERT_EQUAL_UINT8(5, src[1].kind);
    TEST_ASSERT_EQUAL_HEX32(0x10u, src[1].caps);
    TEST_ASSERT_FALSE(src[1].usable);
    TEST_ASSERT_TRUE(src[0].usable);

    KP.zone_learn(KP.ctx, 2, true);
    TEST_ASSERT_TRUE(F.learn_on);
    TEST_ASSERT_EQUAL_UINT8(3, G.learning);
    zsrc_candidate_t c[8];
    TEST_ASSERT_EQUAL(2u, KP.zone_candidates(KP.ctx, 2, "", c, 2)); /* at most what it holds */
    TEST_ASSERT_EQUAL(3u, KP.zone_candidates(KP.ctx, 2, "", c, 8));
    TEST_ASSERT_EQUAL_STRING("Sensor 1", c[1].name);
    TEST_ASSERT_TRUE(c[1].changed);
    TEST_ASSERT_EQUAL_UINT64(102u, c[2].addr.id);

    b.in[ZSRC_ROLE_MAIN].addr = c[1].addr;
    b.in[ZSRC_ROLE_MAIN].map.invert = true;
    b.in[ZSRC_ROLE_MAIN].map.response_ms = 350;
    snprintf(b.in[ZSRC_ROLE_TAMPER].addr.ref, sizeof(b.in[0].addr.ref), "binary_sensor.hall");
    TEST_ASSERT_EQUAL(ZONE_HUB_OK, KP.zone_bind(KP.ctx, 9, &b));
    TEST_ASSERT_EQUAL_UINT16(9, F.bound_zone);
    TEST_ASSERT_EQUAL_MEMORY(&b.in[0].addr.id, &F.bound.in[0].addr.id, sizeof(uint64_t));
    TEST_ASSERT_EQUAL_STRING("binary_sensor.hall", F.bound.in[ZSRC_ROLE_TAMPER].addr.ref);
    TEST_ASSERT_TRUE(F.bound.in[0].map.invert);

    zone_hub_info_t info;
    TEST_ASSERT_TRUE(KP.zone_info(KP.ctx, 9, &info));
    TEST_ASSERT_EQUAL_INT32(-2150, info.value_x100);
    TEST_ASSERT_EQUAL_UINT64(0x0123456789ull, info.heard_at);
    TEST_ASSERT_EQUAL_STRING("Terminal 4", info.address);
    TEST_ASSERT_EQUAL_UINT16(350, info.binding.in[0].map.response_ms);
    TEST_ASSERT_FALSE(KP.zone_info(KP.ctx, 10, &info));

    /* the keypad gone a minute: its learn mode and installer mode end on the alarm panel */
    ui_remote_grant_tick(&MASTER, &G, true, NOW);
    TEST_ASSERT_FALSE(F.learn_on);
    TEST_ASSERT_FALSE(F.program);
    TEST_ASSERT_EQUAL(1, F.exits);
    TEST_ASSERT_EQUAL_UINT8(0, G.learning);
}

/* LINK-063: nothing came back - never a success */
static void test_no_answer(void)
{
    DOWN = true;
    uint8_t user = 7;
    TEST_ASSERT_EQUAL(ALARM_ERR_UNAVAILABLE, KP.program_enter(KP.ctx, "69420", &user));
    TEST_ASSERT_EQUAL_UINT8(0, user);
    TEST_ASSERT_EQUAL(UR_NO_ANSWER, PX.last);
    zone_hub_binding_t b;
    memset(&b, 0, sizeof(b));
    TEST_ASSERT_EQUAL(ZONE_HUB_ERR_BUSY, KP.zone_bind(KP.ctx, 1, &b));
    zone_hub_info_t info;
    TEST_ASSERT_FALSE(KP.zone_info(KP.ctx, 1, &info));
    TEST_ASSERT_FALSE(KP.web_login_get == NULL ? false : KP.web_login_get(KP.ctx, 1));
}

/* LINK-058: the frozen v1 bytes, read and written by today's code */
static void test_fixture_v1(void)
{
    uint8_t user = 0;
    KP.program_enter(KP.ctx, "1234", &user);
    TEST_ASSERT_EQUAL_size_t(sizeof(UR_V1_ENTER_ARGS), ARGS_SEEN_LEN);
    TEST_ASSERT_EQUAL_MEMORY(UR_V1_ENTER_ARGS, ARGS_SEEN, sizeof(UR_V1_ENTER_ARGS));

    /* the answer to a right code (the fake alarm panel's installer code is 69420, its user 1) */
    memset(&G, 0, sizeof(G));
    F.program = false;
    size_t n = 0;
    uint8_t out[16];
    const uint8_t right[] = {0x05, 0x00, '6', '9', '4', '2', '0'};
    TEST_ASSERT_EQUAL(UR_OK, ui_remote_serve(&S, &MASTER, &G, 0, true, UR_PROGRAM_ENTER, right,
                                             sizeof(right), out, sizeof(out), &n, NOW));
    TEST_ASSERT_EQUAL_size_t(sizeof(UR_V1_ENTER_ANSWER), n);
    TEST_ASSERT_EQUAL_MEMORY(UR_V1_ENTER_ANSWER, out, n);

    /* ZONE_BIND: today's decoder reads the frozen bytes as the values the fixture names ... */
    zone_hub_binding_t b;
    TEST_ASSERT_TRUE(ui_remote_dec_binding(UR_V1_BIND_ARGS + 2, sizeof(UR_V1_BIND_ARGS) - 2u, &b));
    TEST_ASSERT_EQUAL_UINT8(2, b.in[0].addr.kind);
    TEST_ASSERT_EQUAL_UINT64(4u, b.in[0].addr.id);
    TEST_ASSERT_TRUE(b.in[0].map.invert);
    TEST_ASSERT_EQUAL_UINT16(60, b.in[0].map.grace_s);
    TEST_ASSERT_EQUAL_UINT16(350, b.in[0].map.response_ms);
    TEST_ASSERT_EQUAL_UINT8(0, b.in[1].addr.kind);
    /* ... and the proxy sends exactly those bytes for them */
    G.installer = true;
    F.program = true;
    TEST_ASSERT_EQUAL(ZONE_HUB_OK, KP.zone_bind(KP.ctx, 5, &b));
    TEST_ASSERT_EQUAL_size_t(sizeof(UR_V1_BIND_ARGS), ARGS_SEEN_LEN);
    TEST_ASSERT_EQUAL_MEMORY(UR_V1_BIND_ARGS, ARGS_SEEN, sizeof(UR_V1_BIND_ARGS));
}

/* LINK-017, LINK-058: a record from a newer sender (more fields) and from an older one (fewer) */
static void test_records_grow(void)
{
    uint8_t buf[256];
    zone_hub_binding_t b, got;
    TEST_ASSERT_TRUE(ui_remote_dec_binding(UR_V1_BIND_ARGS + 2, sizeof(UR_V1_BIND_ARGS) - 2u, &b));

    /* a newer address record: two bytes appended to the main input's address */
    memcpy(buf, UR_V1_BIND_ARGS + 2, sizeof(UR_V1_BIND_ARGS) - 2u);
    size_t len = sizeof(UR_V1_BIND_ARGS) - 2u;
    const size_t addr_at = 2u + 2u; /* binding length, input length */
    memmove(buf + addr_at + 2u + 12u + 2u, buf + addr_at + 2u + 12u, len - (addr_at + 2u + 12u));
    buf[addr_at + 2u + 12u] = 0xEE;
    buf[addr_at + 2u + 13u] = 0xEE;
    buf[addr_at] = 14; /* the address record */
    buf[2] = 0x24;     /* the input record: 36 */
    buf[0] = 0x6E;     /* the binding: 110 */
    len += 2u;
    TEST_ASSERT_TRUE(ui_remote_dec_binding(buf, len, &got));
    TEST_ASSERT_EQUAL_MEMORY(&b, &got, sizeof(b));

    /* an older mapping record (two bytes shorter: no hold_s) reads hold_s as 0, the rest as sent */
    b.in[0].map.hold_s = 77;
    len = ui_remote_enc_binding(&b, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(sizeof(UR_V1_BIND_ARGS) - 2u, len);
    const size_t map_at = 2u + 2u + 14u; /* binding, input, address record */
    memmove(buf + map_at + 18u, buf + map_at + 20u, len - (map_at + 20u));
    buf[map_at] = 16;  /* the mapping record */
    buf[2] = 0x20;     /* the input: 32 */
    buf[0] = 0x6A;     /* the binding: 106 */
    len -= 2u;
    TEST_ASSERT_TRUE(ui_remote_dec_binding(buf, len, &got));
    TEST_ASSERT_EQUAL_UINT16(0, got.in[0].map.hold_s);
    TEST_ASSERT_EQUAL_UINT16(350, got.in[0].map.response_ms);
    TEST_ASSERT_EQUAL_UINT16(60, got.in[0].map.grace_s);
}

/* LINK-058: every field of the structs the calls carry is in their tables. A struct filled with
 * 0x01 (its strings with 'a') goes through the codec; the bytes that come back different are the
 * ones no table field covers: the struct's padding - or a field added to the struct and not to its
 * table, which shows here as more of them. */
static void fill(const ur_table_t *t, uint8_t *obj)
{
    memset(obj, 0x01, t->size);
    for (size_t i = 0; i < t->n; i++) {
        const ur_field_t *f = &t->f[i];
        for (size_t e = 0; e < f->count; e++) {
            uint8_t *p = obj + f->off + e * f->size;
            if (f->kind == UR_F_STR || f->kind == UR_F_SECRET) {
                memset(p, 'a', f->size - 1u);
                p[f->size - 1u] = 0;
            } else if (f->kind == UR_F_REC) {
                fill(f->sub, p);
            }
        }
    }
}

static size_t not_covered(const ur_table_t *t)
{
    static uint8_t a[2048], b2[2048], wire[4096];
    TEST_ASSERT_TRUE(t->size <= sizeof(a));
    fill(t, a);
    plink_w_t w;
    plink_w_init(&w, wire, sizeof(wire));
    ur_put(&w, t, a);
    TEST_ASSERT_TRUE(w.ok);
    plink_r_t r;
    plink_r_init(&r, wire, w.len);
    ur_get(&r, t, b2);
    TEST_ASSERT_TRUE(r.ok);
    size_t n = 0;
    for (size_t i = 0; i < t->size; i++) {
        n += b2[i] != a[i] ? 1u : 0u;
    }
    return n;
}

static void test_tables_cover_the_structs(void)
{
    /* only padding: each input of a binding 14 (6 in its address, 4 + 4 in and after its
     * mapping), a zone's info 8 more, a candidate 6 + 1, a source 3 + 3 */
    TEST_ASSERT_EQUAL_size_t(UR_PAD_BINDING, not_covered(&ur_t_binding));
    TEST_ASSERT_EQUAL_size_t(UR_PAD_INFO, not_covered(&ur_t_info));
    TEST_ASSERT_EQUAL_size_t(UR_PAD_CANDIDATE, not_covered(&ur_t_candidate));
    TEST_ASSERT_EQUAL_size_t(UR_PAD_SOURCE, not_covered(&ur_t_source));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_calls_and_proxy);
    RUN_TEST(test_program_enter_from_a_keypad);
    RUN_TEST(test_installer_mode_one_place);
    RUN_TEST(test_users_need_a_grant);
    RUN_TEST(test_set_config_keeps_codes);
    RUN_TEST(test_zone_added_from_a_keypad);
    RUN_TEST(test_no_answer);
    RUN_TEST(test_fixture_v1);
    RUN_TEST(test_records_grow);
    RUN_TEST(test_tables_cover_the_structs);
    return UNITY_END();
}
