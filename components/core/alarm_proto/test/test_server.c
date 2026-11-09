/*
 * test_server.c - the server link's messages, ceiling and replay queue (alarm-server's
 * docs/features/02-Panel-and-App-API; API-NNN).
 */
#include <stdio.h>
#include <string.h>

#include "alarm_proto/proto_link.h"
#include "alarm_proto/proto_report.h"
#include "alarm_proto/proto_server.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static char buf[4096];

/* API-002: the id and the bytes signed, the same as alarm-server's Go tests. */
static void test_id_and_signed_bytes(void)
{
    uint8_t pub[32];
    memset(pub, 0x11, sizeof(pub));
    char id[PROTO_SRV_ID_LEN];
    proto_srv_id(pub, id);
    TEST_ASSERT_EQUAL_size_t(16u, strlen(id));

    uint8_t out[PROTO_SRV_SIGN_MAX];
    const size_t n = proto_srv_sign_bytes("Cloud.RoboAlarms.com:443", "a1b2c3", 42u, out, sizeof(out));
    static const char WANT[] = "roboalarms-mqtt-v1\0cloud.roboalarms.com\0a1b2c3\0" "42";
    TEST_ASSERT_EQUAL_size_t(sizeof(WANT) - 1u, n);
    TEST_ASSERT_EQUAL_MEMORY(WANT, out, n);
    TEST_ASSERT_EQUAL_size_t(0u, proto_srv_sign_bytes("h", "i", 1u, out, 8u));
}

/* The vector alarm-server's Go tests use (internal/linkproto/testdata/signin.json). */
static void test_golden_signin(void)
{
    static const char PASSWORD[] =
        "42.dgcxHz13QoCGRGvA05630A99YTAq-QMjDdtmHywLsSjy0ZFUaH8pvRhMuW9MzjCdE9muwSYMPCoCV_MFzrBuCw."
        "LPjK8keqGUmHhPZep3gjOedjlgMWFeX4ajLTjVDqM5A";
    uint8_t sig[64];
    uint8_t pub[32];
    TEST_ASSERT_EQUAL_size_t(64u, proto_srv_unb64url(
        "dgcxHz13QoCGRGvA05630A99YTAq-QMjDdtmHywLsSjy0ZFUaH8pvRhMuW9MzjCdE9muwSYMPCoCV_MFzrBuCw", sig,
        sizeof(sig)));
    TEST_ASSERT_EQUAL_size_t(32u, proto_srv_unb64url("LPjK8keqGUmHhPZep3gjOedjlgMWFeX4ajLTjVDqM5A",
                                                     pub, sizeof(pub)));
    char id[PROTO_SRV_ID_LEN];
    proto_srv_id(pub, id);
    TEST_ASSERT_EQUAL_STRING("4ce7aabec9ba574a", id);
    char pw[PROTO_SRV_PASSWORD_LEN];
    TEST_ASSERT_TRUE(proto_srv_password(42u, sig, pub, pw, sizeof(pw)));
    TEST_ASSERT_EQUAL_STRING(PASSWORD, pw);
}

static void test_base64url(void)
{
    char s[32];
    uint8_t back[16];
    /* RFC 4648 §10, unpadded */
    TEST_ASSERT_EQUAL_size_t(6u, proto_srv_b64url((const uint8_t *)"foob", 4u, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("Zm9vYg", s);
    TEST_ASSERT_EQUAL_size_t(4u, proto_srv_b64url((const uint8_t *)"foo", 3u, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("Zm9v", s);
    TEST_ASSERT_EQUAL_size_t(2u, proto_srv_b64url((const uint8_t *)"f", 1u, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("Zg", s);
    const uint8_t hi[] = {0xFBu, 0xFFu};
    TEST_ASSERT_EQUAL_size_t(3u, proto_srv_b64url(hi, 2u, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("-_8", s);
    TEST_ASSERT_EQUAL_size_t(2u, proto_srv_unb64url("-_8", back, sizeof(back)));
    TEST_ASSERT_EQUAL_MEMORY(hi, back, 2u);
    TEST_ASSERT_EQUAL_size_t(0u, proto_srv_unb64url("A", back, sizeof(back)));
    TEST_ASSERT_EQUAL_size_t(0u, proto_srv_unb64url("Zh", back, sizeof(back))); /* stray bits */
    TEST_ASSERT_EQUAL_size_t(0u, proto_srv_unb64url("a=b", back, sizeof(back)));
    TEST_ASSERT_EQUAL_size_t(0u, proto_srv_b64url((const uint8_t *)"foo", 3u, s, 4u));
}

static void test_password(void)
{
    uint8_t sig[64];
    uint8_t pub[32];
    memset(sig, 0, sizeof(sig));
    memset(pub, 0, sizeof(pub));
    char pw[PROTO_SRV_PASSWORD_LEN];
    TEST_ASSERT_TRUE(proto_srv_password(7u, sig, pub, pw, sizeof(pw)));
    TEST_ASSERT_EQUAL_STRING_LEN("7.AAAA", pw, 6);
    TEST_ASSERT_EQUAL_size_t(2u + 86u + 1u + 43u, strlen(pw));
    TEST_ASSERT_FALSE(proto_srv_password(7u, sig, pub, pw, 40u));
}

/* API-005: the ceiling refuses before the engine runs. */
static void test_ceiling(void)
{
    proto_srv_ceiling_t c;
    proto_srv_ceiling_defaults(&c);
    TEST_ASSERT_EQUAL(PROTO_SRV_NOT_ALLOWED, proto_srv_check(&c, 3, PROTO_SRV_ACT_ORDINARY, 1000));
    proto_srv_user_set_remote(&c, 3, true, 0);
    TEST_ASSERT_EQUAL(PROTO_SRV_OK, proto_srv_check(&c, 3, PROTO_SRV_ACT_ORDINARY, 0));
    TEST_ASSERT_EQUAL(PROTO_SRV_OK, proto_srv_check(&c, 3, PROTO_SRV_ACT_DISARM, 0));
    c.remote_disarm = false;
    TEST_ASSERT_EQUAL(PROTO_SRV_REMOTE_OFF, proto_srv_check(&c, 3, PROTO_SRV_ACT_DISARM, 0));
    TEST_ASSERT_EQUAL(PROTO_SRV_OK, proto_srv_check(&c, 3, PROTO_SRV_ACT_ORDINARY, 0));
    c.remote_setup = false;
    TEST_ASSERT_EQUAL(PROTO_SRV_REMOTE_OFF, proto_srv_check(&c, 3, PROTO_SRV_ACT_SETUP, 0));
    c.remote_people = false;
    TEST_ASSERT_EQUAL(PROTO_SRV_REMOTE_OFF, proto_srv_check(&c, 3, PROTO_SRV_ACT_PEOPLE, 0));

    /* An end time holds on the panel's own clock; no clock counts as passed. */
    proto_srv_user_set_remote(&c, 4, true, 5000);
    TEST_ASSERT_EQUAL(PROTO_SRV_OK, proto_srv_check(&c, 4, PROTO_SRV_ACT_ORDINARY, 4999));
    TEST_ASSERT_EQUAL(PROTO_SRV_EXPIRED, proto_srv_check(&c, 4, PROTO_SRV_ACT_ORDINARY, 5000));
    TEST_ASSERT_EQUAL(PROTO_SRV_EXPIRED, proto_srv_check(&c, 4, PROTO_SRV_ACT_ORDINARY, 0));

    proto_srv_user_set_remote(&c, 3, false, 0);
    TEST_ASSERT_EQUAL(PROTO_SRV_NOT_ALLOWED, proto_srv_check(&c, 3, PROTO_SRV_ACT_ORDINARY, 1));
    TEST_ASSERT_EQUAL(PROTO_SRV_NOT_ALLOWED, proto_srv_check(&c, 0, PROTO_SRV_ACT_ORDINARY, 1));
    TEST_ASSERT_EQUAL(PROTO_SRV_NOT_ALLOWED,
                      proto_srv_check(&c, ALARM_MAX_USERS + 1u, PROTO_SRV_ACT_ORDINARY, 1));
    TEST_ASSERT_EQUAL_STRING("remote_off", proto_srv_refusal_name(PROTO_SRV_REMOTE_OFF));
}

static void test_hello_golden(void)
{
    static const char *const CAPS[] = {"walk_test", "sealed_code"};
    const proto_srv_hello_t h = {.id = "abcd",
                                 .model = "M",
                                 .fw = "1.0",
                                 .caps = CAPS,
                                 .caps_n = 2u,
                                 .pub = "P",
                                 .seal_pub = "S",
                                 .seal_sig = "G",
                                 .linked = true,
                                 .boot = 412u,
                                 .uptime_ms = 8812345u,
                                 .now_ms = 1793662800000,
                                 .clock = "rtc",
                                 .tz = "America/Chicago",
                                 .last_seq = 20517u,
                                 .oldest_seq = 14437u,
                                 .path = "wifi"};
    pj_writer_t w;
    pj_init(&w, buf, sizeof(buf));
    proto_srv_hello(&w, &h);
    TEST_ASSERT_NOT_EQUAL_size_t(0u, pj_finish(&w));
    TEST_ASSERT_EQUAL_STRING(
        "{\"t\":\"hello\",\"api\":1,\"id\":\"abcd\",\"model\":\"M\",\"fw\":\"1.0\","
        "\"caps\":[\"walk_test\",\"sealed_code\"],\"pub\":\"P\",\"seal_pub\":\"S\",\"seal_sig\":\"G\",\"linked\":true,"
        "\"boot\":412,\"uptime_ms\":8812345,\"now_ms\":1793662800000,\"clock\":\"rtc\","
        "\"tz\":\"America/Chicago\",\"last_seq\":20517,\"oldest_seq\":14437,\"path\":\"wifi\"}",
        buf);
}

/* F3: the seal key is signed by the link key over "rs1-seal|" and the exact string sent. */
static void test_seal_signed_bytes(void)
{
    char out[64];
    TEST_ASSERT_EQUAL_size_t(13u, proto_srv_seal_signed("abcd", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("rs1-seal|abcd", out);
    TEST_ASSERT_EQUAL_size_t(0u, proto_srv_seal_signed("abcd", out, 13u));
}

/* API-017: the event says what the panel believed and its uptime, so the server can correct it. */
static void test_event_carries_clock(void)
{
    static alarm_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    elog_entry_t e;
    memset(&e, 0, sizeof(e));
    e.boot = 412u;
    e.clock = PROTO_SRV_CLOCK_RTC;
    e.ev.seq = 20513u;
    e.ev.type = ALARM_EV_DISARMED;
    e.ev.mono_ms = 8710044u;
    e.ev.epoch_ms = 1793662697699;
    pj_writer_t w;
    pj_init(&w, buf, sizeof(buf));
    proto_srv_event(&w, &cfg, &e, true, "Disarmed", "by Alex", NULL);
    TEST_ASSERT_NOT_EQUAL_size_t(0u, pj_finish(&w));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"seq\":20513"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"replayed\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"at_ms\":1793662697699"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"boot\":412,\"uptime_ms\":8710044,\"clock\":\"rtc\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"text\":\"Disarmed\",\"body\":\"by Alex\""));

    e.ev.epoch_ms = 0;
    e.clock = PROTO_SRV_CLOCK_NONE;
    pj_init(&w, buf, sizeof(buf));
    proto_srv_event(&w, &cfg, &e, false, NULL, NULL, NULL);
    TEST_ASSERT_NOT_EQUAL_size_t(0u, pj_finish(&w));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"at_ms\":null"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"clock\":\"none\""));
    TEST_ASSERT_NULL(strstr(buf, "words"));
}

/* panel-link.md "event": an event a command or request caused carries its actor, so the server credits
 * the person (a technician working as the installer user), and only those events do. */
static void test_event_carries_its_actor(void)
{
    static alarm_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    elog_entry_t e;
    memset(&e, 0, sizeof(e));
    e.ev.seq = 41u;
    e.ev.type = ALARM_EV_PROGRAM_ENTER;
    e.ev.source = (alarm_source_t){.kind = ALARM_SRC_REMOTE, .id = ALARM_SRC_ID_SERVER_APP};
    proto_srv_actors_t t;
    memset(&t, 0, sizeof(t));
    proto_srv_actor_t tech = {.account = "a_19", .name = "Sam", .via = "website"};
    proto_srv_actors_note(&t, 40u, 42u, &tech); /* the request caused 41 and 42 */
    proto_srv_actors_note(&t, 42u, 42u, &tech); /* one that caused none is not kept */
    const proto_srv_actor_t *a = proto_srv_actors_find(&t, &e.ev);
    TEST_ASSERT_NOT_NULL(a);
    pj_writer_t w;
    pj_init(&w, buf, sizeof(buf));
    proto_srv_event(&w, &cfg, &e, false, NULL, NULL, a);
    TEST_ASSERT_NOT_EQUAL_size_t(0u, pj_finish(&w));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"actor\":{\"account\":\"a_19\",\"name\":\"Sam\",\"via\":\"website\"}"));

    e.ev.seq = 43u; /* after the run */
    TEST_ASSERT_NULL(proto_srv_actors_find(&t, &e.ev));
    e.ev.seq = 42u;
    e.ev.source = (alarm_source_t){.kind = ALARM_SRC_KEYPAD, .id = 1u}; /* someone at the panel, meanwhile */
    TEST_ASSERT_NULL(proto_srv_actors_find(&t, &e.ev));
    e.ev.source = (alarm_source_t){.kind = ALARM_SRC_REMOTE, .id = ALARM_SRC_ID_SERVER_WEB};
    TEST_ASSERT_NOT_NULL(proto_srv_actors_find(&t, &e.ev));

    for (uint32_t i = 0; i < PROTO_SRV_ACTOR_RUNS; i++) /* newer runs push the oldest out */
        proto_srv_actors_note(&t, 100u + i * 2u, 101u + i * 2u, &tech);
    TEST_ASSERT_NULL(proto_srv_actors_find(&t, &e.ev));

    pj_init(&w, buf, sizeof(buf));
    proto_srv_event(&w, &cfg, &e, false, NULL, NULL, NULL);
    TEST_ASSERT_NOT_EQUAL_size_t(0u, pj_finish(&w));
    TEST_ASSERT_NULL(strstr(buf, "actor"));
}

/* The actor of a command and of a request is read with them. */
static void test_actor_is_parsed(void)
{
    proto_srv_in_t in;
    const char *req = "{\"t\":\"req\",\"id\":\"r_1\",\"as\":1,\"actor\":{\"account\":\"a_19\",\"name\":\"Sam\","
                      "\"via\":\"website\",\"device\":\"x\"},\"method\":\"POST\",\"path\":\"setup-mode\",\"body\":{\"open\":true},"
                      "\"fresh\":true}";
    proto_srv_parse(req, strlen(req), false, false, &in);
    TEST_ASSERT_EQUAL(PROTO_SRV_IN_REQ, in.type);
    TEST_ASSERT_EQUAL_STRING("a_19", in.u.req.actor.account);
    TEST_ASSERT_EQUAL_STRING("Sam", in.u.req.actor.name);
    TEST_ASSERT_EQUAL_STRING("website", in.u.req.actor.via);
    const char *cmd = "{\"t\":\"command\",\"id\":\"c_1\",\"as\":2,\"actor\":{\"account\":\"a_7\",\"name\":\"Emma\","
                      "\"via\":\"app\"},\"action\":\"arm\",\"partition\":1,\"level\":\"away\"}";
    proto_srv_parse(cmd, strlen(cmd), false, false, &in);
    TEST_ASSERT_EQUAL(PROTO_SRV_IN_COMMAND, in.type);
    TEST_ASSERT_EQUAL_STRING("a_7", in.u.command.actor_account);
    TEST_ASSERT_EQUAL_STRING("Emma", in.u.command.actor_name);
}

/* internal/linkproto/testdata/result.json */
static void test_golden_result(void)
{
    const proto_srv_result_t r = {.id = "cmd_8f2a", .action = "arm", .result = "not_ready", .user = 2u,
                                  .zone = 2u, .words = "Back Door is open", .partition = 1u};
    pj_writer_t w;
    pj_init(&w, buf, sizeof(buf));
    proto_srv_result(&w, &r);
    TEST_ASSERT_NOT_EQUAL_size_t(0u, pj_finish(&w));
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"result\",\"id\":\"cmd_8f2a\",\"action\":\"arm\","
                             "\"result\":\"not_ready\",\"user\":2,\"zone\":2,"
                             "\"words\":\"Back Door is open\",\"partition\":1}",
                             buf);
}

/* internal/linkproto/testdata/event.json: the same keys and values (order isn't the contract). */
static void test_golden_event(void)
{
    static alarm_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.zones[1].name, sizeof(cfg.zones[1].name), "Back Door");
    elog_entry_t e;
    memset(&e, 0, sizeof(e));
    e.boot = 412u;
    e.clock = PROTO_SRV_CLOCK_RTC;
    e.ev.seq = 20513u;
    e.ev.type = ALARM_EV_ALARM;
    e.ev.partition = 1u;
    e.ev.zone = 2u;
    e.ev.arg = ALARM_CAUSE_BURGLARY;
    e.ev.source.kind = ALARM_SRC_ZONE;
    e.ev.mono_ms = 8800000u;
    e.ev.epoch_ms = 1793662800000;
    pj_writer_t w;
    pj_init(&w, buf, sizeof(buf));
    proto_srv_event(&w, &cfg, &e, true, "Burglary alarm: Back Door", NULL, NULL);
    TEST_ASSERT_NOT_EQUAL_size_t(0u, pj_finish(&w));
    static const char *const WANT[] = {
        "\"t\":\"event\"", "\"seq\":20513", "\"event_type\":\"alarm\"", "\"partition\":1",
        "\"zone\":2", "\"zone_name\":\"Back Door\"", "\"source\":\"zone\"", "\"cause\":\"burglary\"",
        "\"silent\":false", "\"replayed\":true", "\"at_ms\":1793662800000", "\"boot\":412",
        "\"uptime_ms\":8800000", "\"clock\":\"rtc\"", "\"key\":\"alarm\"",
        "\"text\":\"Burglary alarm: Back Door\""};
    for (size_t i = 0; i < sizeof(WANT) / sizeof(WANT[0]); i++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, WANT[i]), WANT[i]);
    }
}

/* API-005: who may use the server, and who the server is told about. Never a code or a hash. */
static void test_people_and_ceiling(void)
{
    static alarm_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.users[1].authority = ALARM_AUTH_MASTER;
    cfg.users[1].partitions = 0x03u;
    cfg.users[1].code_hash[0] = 0xAAu;
    snprintf(cfg.users[1].name, sizeof(cfg.users[1].name), "Alex");
    cfg.users[4].authority = ALARM_AUTH_STANDARD;
    cfg.users[4].partitions = 0x01u;
    snprintf(cfg.users[4].name, sizeof(cfg.users[4].name), "Pat");
    proto_srv_ceiling_t c;
    proto_srv_ceiling_defaults(&c);
    proto_srv_user_set_remote(&c, 2u, true, 0);
    proto_srv_user_set_remote(&c, 5u, true, 5000);
    uint8_t browser[PROTO_SRV_USERS_BYTES] = {0x02u};
    pj_writer_t w;
    pj_init(&w, buf, sizeof(buf));
    proto_srv_people(&w, &cfg, &c, browser);
    TEST_ASSERT_NOT_EQUAL_size_t(0u, pj_finish(&w));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"number\":2,\"name\":\"Alex\",\"authority\":\"master\","
                                     "\"partitions\":[1,2],\"has_code\":true,\"browser\":true,"
                                     "\"remote\":true,\"until_ms\":null"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"number\":5,\"name\":\"Pat\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"has_code\":false,\"browser\":false,\"remote\":true,"
                                     "\"until_ms\":5000"));
    TEST_ASSERT_NULL(strstr(buf, "hash"));

    c.remote_disarm = false;
    c.ask_code = true;
    c.rev = 9u;
    pj_init(&w, buf, sizeof(buf));
    proto_srv_ceiling(&w, &c, true);
    TEST_ASSERT_NOT_EQUAL_size_t(0u, pj_finish(&w));
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"ceiling\",\"rev\":9,\"remote_users\":[2,5],"
                             "\"remote_disarm\":false,\"remote_setup\":true,"
                             "\"remote_people\":true,\"ask_code\":true,\"installer_setup\":true}",
                             buf);
    /* CORE-PROG-014: the Owner turned the Admin code's Setup mode off */
    pj_init(&w, buf, sizeof(buf));
    proto_srv_ceiling(&w, &c, false);
    TEST_ASSERT_NOT_EQUAL_size_t(0u, pj_finish(&w));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"installer_setup\":false}"));
}

static void test_small_messages(void)
{
    pj_writer_t w;
    pj_init(&w, buf, sizeof(buf));
    proto_srv_link_done(&w, "clm_x", true, 2u);
    TEST_ASSERT_NOT_EQUAL_size_t(0u, pj_finish(&w));
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"link_done\",\"claim\":\"clm_x\",\"ok\":true,\"user\":2}", buf);
    pj_init(&w, buf, sizeof(buf));
    proto_srv_link_done(&w, "clm_x", false, 0u);
    TEST_ASSERT_NOT_EQUAL_size_t(0u, pj_finish(&w));
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"link_done\",\"claim\":\"clm_x\",\"ok\":false,\"user\":null}", buf);
    pj_init(&w, buf, sizeof(buf));
    proto_srv_res(&w, "r1", 403, "not_allowed");
    TEST_ASSERT_NOT_EQUAL_size_t(0u, pj_finish(&w));
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"res\",\"id\":\"r1\",\"status\":403,\"body\":{\"error\":\"not_allowed\"}}", buf);
    pj_init(&w, buf, sizeof(buf));
    proto_srv_res(&w, "r2", 200, NULL);
    TEST_ASSERT_NOT_EQUAL_size_t(0u, pj_finish(&w));
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"res\",\"id\":\"r2\",\"status\":200}", buf);
    pj_init(&w, buf, sizeof(buf));
    proto_srv_online(&w, false);
    TEST_ASSERT_NOT_EQUAL_size_t(0u, pj_finish(&w));
    TEST_ASSERT_EQUAL_STRING("{\"online\":false}", buf); /* the last will, byte for byte */
    pj_init(&w, buf, sizeof(buf));
    const proto_srv_health_t h = {.power = "mains", .battery_pct = -1, .rssi = -57, .uptime_s = 96u,
                                  .heap_kb = 100u, .clock = "ntp", .path = "wifi"};
    proto_srv_health(&w, &h);
    TEST_ASSERT_NOT_EQUAL_size_t(0u, pj_finish(&w));
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"health\",\"power\":\"mains\",\"battery_pct\":null,\"rssi\":-57,"
                             "\"uptime_s\":96,\"heap_kb\":100,\"clock\":\"ntp\",\"path\":\"wifi\"}",
                             buf);
    pj_init(&w, buf, sizeof(buf));
    proto_srv_config(&w, "alarm", 7u);
    TEST_ASSERT_NOT_EQUAL_size_t(0u, pj_finish(&w));
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"config\",\"domain\":\"alarm\",\"rev\":7}", buf);
}

/* A full 128-zone panel's snapshot with its panel object fits one message. */
static void test_snapshot_has_panel_and_fits(void)
{
    static alarm_config_t cfg;
    static alarm_snapshot_t snap;
    static char big[PROTO_LINK_SNAPSHOT_MAX];
    memset(&cfg, 0, sizeof(cfg));
    memset(&snap, 0, sizeof(snap));
    const proto_when_t when = {.now = 1000u, .epoch_ms = 1793662800000, .stamp = true, .boot = 4u};
    const proto_srv_panel_t panel = {.name = "Home", .tz = "America/Chicago", .region = "US", .fw = "1.0",
                                     .power = "mains", .battery_pct = 80, .setup_mode = true};
    pj_writer_t w;
    pj_init(&w, big, sizeof(big));
    proto_srv_snapshot(&w, &cfg, &snap, &when, &panel);
    TEST_ASSERT_NOT_EQUAL_size_t(0u, pj_finish(&w));
    TEST_ASSERT_NOT_NULL(strstr(big, "\"t\":\"snapshot\""));
    /* When it was written, on the hello's clock: the server dates countdowns from it. */
    TEST_ASSERT_NOT_NULL(strstr(big, "\"seq\":0,\"boot\":4,\"uptime_ms\":1000,"));
    TEST_ASSERT_NOT_NULL(strstr(big, "\"panel\":{\"name\":\"Home\",\"tz\":\"America/Chicago\",\"region\":\"US\","
                                     "\"fw\":\"1.0\",\"power\":\"mains\",\"battery\":80,\"setup_mode\":true}"));
}

/* A delta says when it was written too, and each partition its time left unrounded, so the
 * server never gives a phone time the panel doesn't have; the Home Assistant link's stay as they were. */
static void test_delta_says_when(void)
{
    static alarm_config_t cfg;
    static alarm_snapshot_t snap;
    static char buf[4096];
    memset(&cfg, 0, sizeof(cfg));
    memset(&snap, 0, sizeof(snap));
    cfg.partitions[0].enabled = true;
    snap.taken_ms = 5000u;
    snap.part[0].state = ALARM_STATE_EXIT_DELAY;
    snap.part[0].delay_remaining_ms = 30000u;
    const proto_when_t when = {.now = 5250u, .epoch_ms = 0, .stamp = true, .boot = 9u};
    pj_writer_t w;
    pj_init(&w, buf, sizeof(buf));
    proto_link_delta(&w, &cfg, &snap, &when, 0x01u, NULL, false);
    TEST_ASSERT_NOT_EQUAL_size_t(0u, pj_finish(&w));
    TEST_ASSERT_NOT_NULL(strstr(buf, "{\"t\":\"delta\",\"seq\":0,\"boot\":9,\"uptime_ms\":5250,"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"delay_remaining_s\":30,\"delay_left_ms\":29750,"));
    const proto_when_t plain = {.now = 5250u, .epoch_ms = 0};
    pj_init(&w, buf, sizeof(buf));
    proto_link_delta(&w, &cfg, &snap, &plain, 0x01u, NULL, false);
    TEST_ASSERT_NOT_EQUAL_size_t(0u, pj_finish(&w));
    TEST_ASSERT_NULL(strstr(buf, "uptime_ms"));
    TEST_ASSERT_NULL(strstr(buf, "delay_left_ms"));
}

/* Hostile input stays cheap and refused: nothing here may run a command. */
static void test_parse_hostile(void)
{
    static proto_srv_in_t in;
    static const char *const BAD[] = {
        "", "{", "}", "null", "{\"t\":1}", "{\"t\":\"command\",\"id\":5,\"action\":\"arm\"}",
        "{\"t\":\"command\",\"id\":\"x\"}", "{\"t\":\"req\",\"id\":\"x\"}",
        "{\"t\":\"people_link\",\"id\":\"x\"}", "{\"t\":\"link_code\"}", "{\"t\":\"link_claim\"}",
        "{\"t\":\"linked\"}", "{\"t\":\"command\",\"id\":\"x\",\"action\":\"arm\"} trailing",
        "{\"t\":\"command\",\"id\":\"x\",\"action\":\"arm\",}",
    };
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        proto_srv_parse(BAD[i], strlen(BAD[i]), false, false, &in);
        TEST_ASSERT_TRUE_MESSAGE(in.type == PROTO_SRV_IN_BAD || in.type == PROTO_SRV_IN_UNKNOWN, BAD[i]);
    }
    proto_srv_parse("{\"seq\":-1}", 9u, true, false, &in);
    TEST_ASSERT_EQUAL(PROTO_SRV_IN_BAD, in.type);
    proto_srv_parse("{\"server_ms\":0}", 14u, false, true, &in);
    TEST_ASSERT_EQUAL(PROTO_SRV_IN_BAD, in.type);
    /* A number too large for the field reads as absent, never wraps round to another user. */
    static const char WRAP[] = "{\"t\":\"command\",\"id\":\"x\",\"action\":\"arm\",\"as\":258}";
    proto_srv_parse(WRAP, strlen(WRAP), false, false, &in);
    TEST_ASSERT_EQUAL(PROTO_SRV_IN_COMMAND, in.type);
    TEST_ASSERT_EQUAL_UINT8(0u, in.u.command.as);
    /* Strings that don't fit are dropped. */
    static char lng[600];
    memset(lng, 'a', sizeof(lng));
    static const char HEAD[] = "{\"t\":\"command\",\"action\":\"arm\",\"id\":\"";
    memcpy(lng, HEAD, sizeof(HEAD) - 1u);
    memcpy(lng + 500, "\"}", 3);
    proto_srv_parse(lng, strlen(lng), false, false, &in);
    TEST_ASSERT_EQUAL(PROTO_SRV_IN_BAD, in.type);
}

/* A short buffer never gives a partial message, and a topic that doesn't fit says so. */
static void test_overflow_and_topics(void)
{
    char tiny[10];
    pj_writer_t w;
    pj_init(&w, tiny, sizeof(tiny));
    proto_srv_res(&w, "a-long-request-id", 200, NULL);
    TEST_ASSERT_EQUAL_size_t(0u, pj_finish(&w));
    char t[64];
    TEST_ASSERT_TRUE(proto_srv_topic("abc", "/up/event", t, sizeof(t)));
    TEST_ASSERT_EQUAL_STRING("ra/abc/up/event", t);
    TEST_ASSERT_FALSE(proto_srv_topic("abc", "/up/event", t, 8u));
    char host[16];
    TEST_ASSERT_TRUE(proto_srv_host_norm("HOST:8443", host, sizeof(host)));
    TEST_ASSERT_EQUAL_STRING("host", host);
    TEST_ASSERT_FALSE(proto_srv_host_norm("", host, sizeof(host)));
    TEST_ASSERT_FALSE(proto_srv_host_norm("a-very-long-host-name", host, sizeof(host)));
}

static void test_refusal_names_and_clock_names(void)
{
    TEST_ASSERT_EQUAL_STRING("not_allowed", proto_srv_refusal_name(PROTO_SRV_NOT_ALLOWED));
    TEST_ASSERT_EQUAL_STRING("expired", proto_srv_refusal_name(PROTO_SRV_EXPIRED));
    TEST_ASSERT_EQUAL_STRING("rtc", proto_srv_clock_name(PROTO_SRV_CLOCK_RTC));
    TEST_ASSERT_EQUAL_STRING("ntp", proto_srv_clock_name(PROTO_SRV_CLOCK_NTP));
    TEST_ASSERT_EQUAL_STRING("manual", proto_srv_clock_name(PROTO_SRV_CLOCK_MANUAL));
    TEST_ASSERT_EQUAL_STRING("server", proto_srv_clock_name(PROTO_SRV_CLOCK_SERVER));
    TEST_ASSERT_EQUAL_STRING("none", proto_srv_clock_name(PROTO_SRV_CLOCK_NONE));
    TEST_ASSERT_EQUAL_STRING("none", proto_srv_clock_name(200u));
}

static void parse(const char *json, bool ack, bool time, proto_srv_in_t *in)
{
    proto_srv_parse(json, strlen(json), ack, time, in);
}

static void test_parse(void)
{
    static proto_srv_in_t in;
    parse("{\"server_ms\":1793673612480,\"echo_uptime_ms\":8812345,\"ack_seq\":20511,"
          "\"location_id\":\"loc_1\",\"ceiling_rev\":7}", false, true, &in);
    TEST_ASSERT_EQUAL(PROTO_SRV_IN_TIME, in.type);
    TEST_ASSERT_EQUAL_INT64(1793673612480, in.u.time.server_ms);
    TEST_ASSERT_EQUAL_UINT32(20511u, in.u.time.ack_seq);
    TEST_ASSERT_EQUAL_STRING("loc_1", in.u.time.location_id);
    parse("{\"server_ms\":5,\"location_id\":null}", false, true, &in);
    TEST_ASSERT_EQUAL(PROTO_SRV_IN_TIME, in.type);
    TEST_ASSERT_EQUAL_STRING("", in.u.time.location_id);

    parse("{\"seq\":12}", true, false, &in);
    TEST_ASSERT_EQUAL(PROTO_SRV_IN_ACK, in.type);
    TEST_ASSERT_EQUAL_UINT32(12u, in.u.ack_seq);

    parse("{\"t\":\"link_code\",\"code\":\"7KQ2MZ\",\"url\":\"https://h/link#7KQ2MZ\","
          "\"expires_s\":600}", false, false, &in);
    TEST_ASSERT_EQUAL(PROTO_SRV_IN_LINK_CODE, in.type);
    TEST_ASSERT_EQUAL_STRING("7KQ2MZ", in.u.link_code.code);
    TEST_ASSERT_EQUAL_UINT32(600u, in.u.link_code.expires_s);

    parse("{\"t\":\"link_claim\",\"claim\":\"clm_x\",\"account_name\":\"Alex\","
          "\"account_email_masked\":\"a***@example.org\",\"location_name\":\"Home\"}", false, false,
          &in);
    TEST_ASSERT_EQUAL(PROTO_SRV_IN_LINK_CLAIM, in.type);
    TEST_ASSERT_EQUAL_STRING("clm_x", in.u.link_claim.claim);
    TEST_ASSERT_EQUAL_STRING("a***@example.org", in.u.link_claim.account_email_masked);

    parse("{\"id\":\"cmd_x\",\"as\":3,\"actor\":{\"account\":\"a\",\"name\":\"Pat\",\"via\":\"app\"},"
          "\"action\":\"arm\",\"partition\":1,\"level\":\"away\",\"bypass_open\":true,"
          "\"sealed_code\":null,\"t\":\"command\",\"later\":{\"a\":[1,2]}}", false, false, &in);
    TEST_ASSERT_EQUAL(PROTO_SRV_IN_COMMAND, in.type);
    TEST_ASSERT_EQUAL_UINT8(3u, in.u.command.as);
    TEST_ASSERT_EQUAL_STRING("Pat", in.u.command.actor_name);
    TEST_ASSERT_EQUAL_STRING("app", in.u.command.actor_via);
    TEST_ASSERT_TRUE(in.u.command.bypass_open);
    TEST_ASSERT_EQUAL_STRING("", in.u.command.sealed_code);

    static const char REQ[] = "{\"t\":\"req\",\"id\":\"r1\",\"as\":2,\"method\":\"POST\","
                              "\"path\":\"people\",\"body\":{\"n\":[1,\"}\"]},\"fresh\":true}";
    parse(REQ, false, false, &in);
    TEST_ASSERT_EQUAL(PROTO_SRV_IN_REQ, in.type);
    TEST_ASSERT_TRUE(in.u.req.fresh);
    TEST_ASSERT_EQUAL_STRING_LEN("{\"n\":[1,\"}\"]}", in.u.req.body, in.u.req.body_len);
    TEST_ASSERT_EQUAL_size_t(strlen("{\"n\":[1,\"}\"]}"), in.u.req.body_len);

    parse("{\"t\":\"people_link\",\"id\":\"pl\",\"user\":2,\"account\":\"a\",\"on\":true,"
          "\"until_ms\":null}", false, false, &in);
    TEST_ASSERT_EQUAL(PROTO_SRV_IN_PEOPLE_LINK, in.type);
    TEST_ASSERT_EQUAL_UINT8(2u, in.u.people_link.user);
    TEST_ASSERT_TRUE(in.u.people_link.on);
    TEST_ASSERT_EQUAL_INT64(0, in.u.people_link.until_ms);

    parse("{\"t\":\"later\"}", false, false, &in);
    TEST_ASSERT_EQUAL(PROTO_SRV_IN_UNKNOWN, in.type);
    parse("{\"t\":\"unlink\"}", false, false, &in);
    TEST_ASSERT_EQUAL(PROTO_SRV_IN_UNLINK, in.type);
    parse("{\"t\":\"command\",", false, false, &in);
    TEST_ASSERT_EQUAL(PROTO_SRV_IN_BAD, in.type);
    parse("{\"t\":\"command\"}", false, false, &in);
    TEST_ASSERT_EQUAL(PROTO_SRV_IN_BAD, in.type);
    parse("[1]", false, false, &in);
    TEST_ASSERT_EQUAL(PROTO_SRV_IN_BAD, in.type);
}

/* API-010 (security review): a sealed code is good for the command it was typed for, now, and
 * nothing else - a kept copy can't disarm later or clear another partition. */
static void test_sealed_code_is_bound(void)
{
    proto_srv_command_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.action, sizeof(c.action), "disarm");
    c.partition = 1u;
    char code[ALARM_CODE_MAX_LEN + 1];
    const int64_t now = 1793500000000;
    TEST_ASSERT_TRUE(proto_srv_sealed_code("rs1|1793500000000|disarm|1|123456", now, &c, code, sizeof(code)));
    TEST_ASSERT_EQUAL_STRING("123456", code);
    TEST_ASSERT_TRUE(proto_srv_sealed_code("rs1|1793499890000|disarm|1|1234", now, &c, code, sizeof(code)));
    TEST_ASSERT_FALSE(proto_srv_sealed_code("rs1|1793499870000|disarm|1|1234", now, &c, code, sizeof(code))); /* too old */
    TEST_ASSERT_FALSE(proto_srv_sealed_code("rs1|1793500130000|disarm|1|1234", now, &c, code, sizeof(code))); /* future */
    TEST_ASSERT_FALSE(proto_srv_sealed_code("rs1|1793500000000|clear|1|1234", now, &c, code, sizeof(code)));
    TEST_ASSERT_FALSE(proto_srv_sealed_code("rs1|1793500000000|disarm|2|1234", now, &c, code, sizeof(code)));
    TEST_ASSERT_FALSE(proto_srv_sealed_code("1234", now, &c, code, sizeof(code))); /* the old, unbound form */
    TEST_ASSERT_EQUAL_STRING("", code);
    TEST_ASSERT_FALSE(proto_srv_sealed_code("rs1|1793500000000|disarm|1|123", now, &c, code, sizeof(code)));
    TEST_ASSERT_FALSE(proto_srv_sealed_code("rs1|1793500000000|disarm|1|12345678901", now, &c, code, sizeof(code)));
    TEST_ASSERT_FALSE(proto_srv_sealed_code("rs1|1793500000000|disarm|1|12a4", now, &c, code, sizeof(code)));
    TEST_ASSERT_FALSE(proto_srv_sealed_code("rs1|1793500000000|disarm|1", now, &c, code, sizeof(code)));
    TEST_ASSERT_FALSE(proto_srv_sealed_code("rs1||disarm|1|1234", now, &c, code, sizeof(code)));
    TEST_ASSERT_FALSE(proto_srv_sealed_code("rs1|1793500000000|disarm|1|1234", 0, &c, code, sizeof(code))); /* no clock */
}

/* MOB-044: a person's code from the app is bound to the slot it is for (set_code and the user's
 * number) or to a new person (create, 0), within two minutes. */
static void test_sealed_code_for_a_slot(void)
{
    char code[ALARM_CODE_MAX_LEN + 1];
    const int64_t now = 1793500000000;
    TEST_ASSERT_TRUE(proto_srv_sealed_for("rs1|1793500000000|set_code|7|482916", now, "set_code", 7u, code, sizeof(code)));
    TEST_ASSERT_EQUAL_STRING("482916", code);
    TEST_ASSERT_FALSE(proto_srv_sealed_for("rs1|1793500000000|set_code|7|482916", now, "set_code", 8u, code, sizeof(code)));
    TEST_ASSERT_FALSE(proto_srv_sealed_for("rs1|1793500000000|create|0|482916", now, "set_code", 0u, code, sizeof(code)));
    TEST_ASSERT_TRUE(proto_srv_sealed_for("rs1|1793500000000|create|0|4829", now, "create", 0u, code, sizeof(code)));
    TEST_ASSERT_EQUAL_STRING("4829", code);
    TEST_ASSERT_FALSE(proto_srv_sealed_for("rs1|1793499870000|create|0|4829", now, "create", 0u, code, sizeof(code)));
    TEST_ASSERT_FALSE(proto_srv_sealed_for("rs1|1793500000000|disarm|1|4829", now, "set_code", 1u, code, sizeof(code)));
}

/* MOB-044: the server's POST people with a sealed code, read; any other action is left alone. */
static void test_people_request_parse(void)
{
    proto_srv_people_t p;
    const char *create = "{\"action\":\"create\",\"name\":\"Pat\",\"role\":\"area_manager\","
                         "\"partitions\":[1,3],\"sealed_code\":\"c2VhbGVk\"}";
    proto_srv_people_parse(create, strlen(create), &p);
    TEST_ASSERT_EQUAL(PROTO_SRV_PEOPLE_CREATE, p.kind);
    TEST_ASSERT_EQUAL_STRING("Pat", p.name);
    TEST_ASSERT_EQUAL(ALARM_AUTH_PARTITION_MASTER, p.authority);
    TEST_ASSERT_EQUAL_HEX8(0x05, p.partitions);
    TEST_ASSERT_EQUAL_STRING("c2VhbGVk", p.sealed_code);

    const char *set = "{\"action\":\"set_code\",\"number\":5,\"sealed_code\":\"c2VhbGVk\"}";
    proto_srv_people_parse(set, strlen(set), &p);
    TEST_ASSERT_EQUAL(PROTO_SRV_PEOPLE_SET_CODE, p.kind);
    TEST_ASSERT_EQUAL_UINT8(5, p.number);

    const char *other = "{\"number\":5,\"action\":\"delete\"}";
    proto_srv_people_parse(other, strlen(other), &p);
    TEST_ASSERT_EQUAL(PROTO_SRV_PEOPLE_OTHER, p.kind);

    static const char *const BAD[] = {
        "{\"action\":\"set_code\",\"number\":5}",                                   /* no code */
        "{\"action\":\"set_code\",\"number\":0,\"sealed_code\":\"x\"}",           /* no slot */
        "{\"action\":\"create\",\"name\":\"Pat\",\"partitions\":[],\"sealed_code\":\"x\"}",
        "{\"action\":\"create\",\"name\":\"Pat\",\"partitions\":[9],\"sealed_code\":\"x\"}",
        "{\"action\":\"create\",\"partitions\":[1],\"sealed_code\":\"x\"}",       /* no name */
        "{\"action\":\"set_code\",\"number\":5,\"sealed_code\":\"x\"",            /* cut short */
    };
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        proto_srv_people_parse(BAD[i], strlen(BAD[i]), &p);
        TEST_ASSERT_EQUAL_MESSAGE(PROTO_SRV_PEOPLE_BAD, p.kind, BAD[i]);
        TEST_ASSERT_EQUAL_STRING("", p.sealed_code);
    }
    TEST_ASSERT_EQUAL(ALARM_AUTH_MASTER, proto_srv_role_authority("owner"));
    TEST_ASSERT_EQUAL(ALARM_AUTH_INSTALLER, proto_srv_role_authority("admin"));
    TEST_ASSERT_EQUAL(ALARM_AUTH_STANDARD, proto_srv_role_authority("someone"));
}

/* Security review: a sealed code never takes "clear" (or anything but a disarm) past its state
 * check, and "ask for code" still holds a disarm without one. */
static void test_command_run(void)
{
    TEST_ASSERT_EQUAL(PROTO_SRV_RUN_AS_USER, proto_srv_command_run(PROTO_SRV_ACT_ORDINARY, true, true));
    TEST_ASSERT_EQUAL(PROTO_SRV_RUN_AS_USER, proto_srv_command_run(PROTO_SRV_ACT_ORDINARY, false, true));
    TEST_ASSERT_EQUAL(PROTO_SRV_RUN_WITH_CODE, proto_srv_command_run(PROTO_SRV_ACT_DISARM, true, true));
    TEST_ASSERT_EQUAL(PROTO_SRV_RUN_WITH_CODE, proto_srv_command_run(PROTO_SRV_ACT_DISARM, false, true));
    TEST_ASSERT_EQUAL(PROTO_SRV_RUN_CODE_REQUIRED, proto_srv_command_run(PROTO_SRV_ACT_DISARM, true, false));
    TEST_ASSERT_EQUAL(PROTO_SRV_RUN_AS_USER, proto_srv_command_run(PROTO_SRV_ACT_DISARM, false, false));

    proto_srv_command_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.action, sizeof(c.action), "clear");
    c.partition = 1u;
    alarm_cmd_t cmd;
    proto_srv_when_t when;
    proto_srv_act_t act;
    TEST_ASSERT_TRUE(proto_srv_command_map(&c, &cmd, &when, &act));
    TEST_ASSERT_EQUAL(PROTO_SRV_WHEN_DISARMED, when); /* the check a sealed code used to skip */
    TEST_ASSERT_EQUAL(PROTO_SRV_RUN_AS_USER, proto_srv_command_run(act, true, true));
}

/* Security review: the tunnel can't arm or disarm (that goes as a command, past the disarm ceiling
 * and the code), and changes people or Setup only with the server's fresh step-up. */
static void test_req_rules(void)
{
    TEST_ASSERT_EQUAL_STRING("use_command", proto_srv_req_refusal("POST", "partition", true));
    TEST_ASSERT_EQUAL_STRING("use_command", proto_srv_req_refusal("POST", "partition?x=1", true));
    TEST_ASSERT_NULL(proto_srv_req_refusal("GET", "partition", false));
    TEST_ASSERT_NULL(proto_srv_req_refusal("POST", "partitions", false));
    TEST_ASSERT_EQUAL_STRING("step_up", proto_srv_req_refusal("POST", "people", false));
    TEST_ASSERT_EQUAL_STRING("step_up", proto_srv_req_refusal("POST", "setup/people", false));
    TEST_ASSERT_EQUAL_STRING("step_up", proto_srv_req_refusal("POST", "setup/alarm", false));
    TEST_ASSERT_NULL(proto_srv_req_refusal("POST", "people", true));
    TEST_ASSERT_NULL(proto_srv_req_refusal("GET", "people", false));
    TEST_ASSERT_NULL(proto_srv_req_refusal("POST", "devices/act", false));
    TEST_ASSERT_EQUAL(PROTO_SRV_ACT_PEOPLE, proto_srv_req_act("POST", "people"));
    TEST_ASSERT_EQUAL(PROTO_SRV_ACT_SETUP, proto_srv_req_act("POST", "zwave/include"));
    /* Setting the installer or master code without the old one: Setup mode's, behind the
     * remote_setup switch and the step-up (the Owner reaches it too, CORE-PROG-001). */
    TEST_ASSERT_EQUAL(PROTO_SRV_ACT_SETUP, proto_srv_req_act("POST", "installer-code"));
    TEST_ASSERT_EQUAL(PROTO_SRV_ACT_SETUP, proto_srv_req_act("POST", "setup-mode"));
    TEST_ASSERT_EQUAL_STRING("step_up", proto_srv_req_refusal("POST", "installer-code", false));
    TEST_ASSERT_EQUAL_STRING("step_up", proto_srv_req_refusal("POST", "setup-mode", false));
    TEST_ASSERT_EQUAL(PROTO_SRV_ACT_ORDINARY, proto_srv_req_act("GET", "people"));
}

static void test_command_map(void)
{
    proto_srv_command_t c;
    memset(&c, 0, sizeof(c));
    alarm_cmd_t cmd;
    proto_srv_when_t when;
    proto_srv_act_t act;
    snprintf(c.action, sizeof(c.action), "arm");
    c.partition = 1u;
    TEST_ASSERT_FALSE(proto_srv_command_map(&c, &cmd, &when, &act)); /* no level */
    snprintf(c.level, sizeof(c.level), "away");
    c.silent_exit = true;
    TEST_ASSERT_TRUE(proto_srv_command_map(&c, &cmd, &when, &act));
    TEST_ASSERT_EQUAL(ALARM_CMD_ARM, cmd.type);
    TEST_ASSERT_EQUAL(ALARM_LEVEL_AWAY, cmd.level);
    TEST_ASSERT_EQUAL(ALARM_ARM_SILENT_EXIT, cmd.arm_flags);
    TEST_ASSERT_EQUAL(PROTO_SRV_WHEN_DISARMED, when);
    TEST_ASSERT_EQUAL(PROTO_SRV_ACT_ORDINARY, act);

    snprintf(c.action, sizeof(c.action), "disarm");
    TEST_ASSERT_TRUE(proto_srv_command_map(&c, &cmd, &when, &act));
    TEST_ASSERT_EQUAL(PROTO_SRV_WHEN_NOT_DISARMED, when);
    TEST_ASSERT_EQUAL(PROTO_SRV_ACT_DISARM, act);

    snprintf(c.action, sizeof(c.action), "silence");
    TEST_ASSERT_TRUE(proto_srv_command_map(&c, &cmd, &when, &act));
    TEST_ASSERT_EQUAL(ALARM_CMD_SILENCE, cmd.type);
    TEST_ASSERT_EQUAL(PROTO_SRV_ACT_DISARM, act);

    snprintf(c.action, sizeof(c.action), "bypass");
    TEST_ASSERT_FALSE(proto_srv_command_map(&c, &cmd, &when, &act)); /* no zone */
    c.zone = 5u;
    TEST_ASSERT_TRUE(proto_srv_command_map(&c, &cmd, &when, &act));
    TEST_ASSERT_EQUAL_UINT16(5u, cmd.zone);

    snprintf(c.action, sizeof(c.action), "walk_test_start");
    TEST_ASSERT_TRUE(proto_srv_command_map(&c, &cmd, &when, &act));
    TEST_ASSERT_EQUAL(ALARM_WALK_START, cmd.walk_op);

    snprintf(c.action, sizeof(c.action), "panic"); /* never from the server */
    TEST_ASSERT_FALSE(proto_srv_command_map(&c, &cmd, &when, &act));
    snprintf(c.action, sizeof(c.action), "arm");
    c.partition = 0u;
    TEST_ASSERT_FALSE(proto_srv_command_map(&c, &cmd, &when, &act));
}

/* API-008: the same id within ten minutes gets the same answer. */
static void test_seen(void)
{
    static proto_srv_seen_t s;
    memset(&s, 0, sizeof(s));
    TEST_ASSERT_NULL(proto_srv_seen_find(&s, "a", 100u));
    proto_srv_seen_add(&s, "a", "{r}", 3u, 100u);
    const proto_srv_seen_entry_t *e = proto_srv_seen_find(&s, "a", 200u);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("{r}", e->res);
    TEST_ASSERT_NULL(proto_srv_seen_find(&s, "b", 200u));
    TEST_ASSERT_NULL(proto_srv_seen_find(&s, "a", 100u + PROTO_SRV_SEEN_MS));
    for (unsigned i = 0; i < PROTO_SRV_SEEN_N; i++) {
        proto_srv_seen_add(&s, "z", "", 0u, 200u);
    }
    TEST_ASSERT_NULL(proto_srv_seen_find(&s, "a", 300u));
}

/* API-018 */
static void test_clock(void)
{
    proto_srv_clock_t c;
    proto_srv_clock_check(1000000, 5000u, 5100u, 1000050, &c);
    TEST_ASSERT_EQUAL_UINT64(100u, c.round_trip_ms);
    TEST_ASSERT_EQUAL_INT64(1000050, c.server_now);
    TEST_ASSERT_EQUAL_INT64(0, c.offset_ms);
    TEST_ASSERT_FALSE(c.set_clock);
    proto_srv_clock_check(1000000, 5000u, 5100u, 1003051, &c); /* 3 s fast */
    TEST_ASSERT_EQUAL_INT64(-3001, c.offset_ms);
    TEST_ASSERT_TRUE(c.set_clock);
    proto_srv_clock_check(1000000, 5000u, 5100u, 0, &c); /* no clock */
    TEST_ASSERT_TRUE(c.set_clock);
    TEST_ASSERT_EQUAL_INT64(1000050, c.server_now);
    /* An old echo (a second time long after the hello) measures nothing: the clock stays. */
    proto_srv_clock_check(1000000, 5000u, 53000u, 999000, &c);
    TEST_ASSERT_FALSE(c.trusted);
    TEST_ASSERT_FALSE(c.set_clock);
}

/* ---- the replay queue: a fake log of events, index = position, seq = index + 100 --------------- */

#define LOG_N 64u
static elog_entry_t s_log[LOG_N];
static uint32_t s_newest;
static uint32_t s_oldest;
static uint32_t s_published[256];
static bool s_replayed[256];
static size_t s_pub_n;
static bool s_pub_ok;

static void log_reset(uint32_t n)
{
    memset(s_log, 0, sizeof(s_log));
    s_newest = n;
    s_oldest = 1u;
    for (uint32_t i = 1; i <= n; i++) {
        s_log[i].index = i;
        s_log[i].kind = ELOG_KIND_EVENT;
        s_log[i].ev.seq = i + 100u;
    }
    s_pub_n = 0;
    s_pub_ok = true;
}

static size_t fake_read(void *ctx, uint32_t after, elog_entry_t *out, size_t max, uint32_t scan,
                        uint32_t *next)
{
    (void)ctx;
    (void)max;
    (void)scan;
    uint32_t i = after + 1u;
    if (i < s_oldest) {
        i = s_oldest;
    }
    if (i > s_newest) {
        *next = after;
        return 0u;
    }
    *out = s_log[i];
    *next = i;
    return 1u;
}

static uint32_t fake_newest(void *ctx)
{
    (void)ctx;
    return s_newest;
}

static bool fake_publish(void *ctx, const elog_entry_t *e, bool replayed)
{
    (void)ctx;
    if (!s_pub_ok) {
        return false;
    }
    s_replayed[s_pub_n] = replayed;
    s_published[s_pub_n++] = e->ev.seq;
    return true;
}

static const proto_srvq_port_t PORT = {NULL, fake_read, fake_newest, fake_publish};

static void drain(proto_srvq_t *q, alarm_ms_t now)
{
    for (int i = 0; i < 200 && proto_srvq_poll(q, now); i++) {
    }
}

/* API-006: nothing goes out before down/time; then in order, as replayed; only the ack moves on. */
static void test_queue_replay_after_outage(void)
{
    log_reset(5u);
    proto_srvq_t q;
    proto_srvq_init(&q, &PORT, 2u, 102u, true, 0u);
    proto_srvq_connected(&q, 10u);
    drain(&q, 10u);
    TEST_ASSERT_EQUAL_size_t(0u, s_pub_n);
    proto_srvq_time(&q, 102u, 20u);
    drain(&q, 20u);
    TEST_ASSERT_EQUAL_size_t(3u, s_pub_n);
    TEST_ASSERT_EQUAL_UINT32(103u, s_published[0]);
    TEST_ASSERT_EQUAL_UINT32(105u, s_published[2]);
    TEST_ASSERT_TRUE(s_replayed[0]);
    TEST_ASSERT_EQUAL_UINT32(2u, q.acked);

    proto_srvq_ack(&q, 104u, 30u);
    TEST_ASSERT_EQUAL_UINT32(4u, q.acked);
    TEST_ASSERT_EQUAL_UINT32(104u, q.acked_seq);
    TEST_ASSERT_TRUE(q.acked_dirty);

    /* A reconnect sends what wasn't acknowledged again; a new one is live. */
    proto_srvq_disconnected(&q, 40u);
    s_pub_n = 0;
    proto_srvq_connected(&q, 50u);
    s_newest = 6u;
    s_log[6].index = 6u;
    s_log[6].kind = ELOG_KIND_EVENT;
    s_log[6].ev.seq = 106u;
    proto_srvq_time(&q, 104u, 60u);
    drain(&q, 60u);
    TEST_ASSERT_EQUAL_size_t(2u, s_pub_n);
    TEST_ASSERT_EQUAL_UINT32(105u, s_published[0]);
    TEST_ASSERT_TRUE(s_replayed[0]);
    TEST_ASSERT_FALSE(s_replayed[1]);
}

/* ack_seq 0: the server has nothing for this panel and the panel keeps its place, no whole log. */
static void test_queue_time_zero_keeps_position(void)
{
    log_reset(5u);
    proto_srvq_t q;
    proto_srvq_init(&q, &PORT, 4u, 104u, true, 0u);
    proto_srvq_connected(&q, 1u);
    proto_srvq_time(&q, 0u, 2u);
    drain(&q, 2u);
    TEST_ASSERT_EQUAL_size_t(1u, s_pub_n);
    TEST_ASSERT_EQUAL_UINT32(105u, s_published[0]);
}

/* A first link starts from now (NOTE-011). */
static void test_queue_first_link_from_now(void)
{
    log_reset(5u);
    proto_srvq_t q;
    proto_srvq_init(&q, &PORT, 0u, 0u, false, 0u);
    proto_srvq_enable(&q, true, 1u);
    proto_srvq_start_from_now(&q, 105u);
    proto_srvq_connected(&q, 2u);
    proto_srvq_time(&q, 0u, 3u);
    drain(&q, 3u);
    TEST_ASSERT_EQUAL_size_t(0u, s_pub_n);
}

/* The server lost events (restored from a backup): it gets what the log still has. */
static void test_queue_server_lost_events(void)
{
    log_reset(10u);
    proto_srvq_t q;
    proto_srvq_init(&q, &PORT, 10u, 110u, true, 0u);
    proto_srvq_connected(&q, 1u);
    proto_srvq_time(&q, 107u, 2u);
    drain(&q, 2u);
    TEST_ASSERT_EQUAL_size_t(3u, s_pub_n);
    TEST_ASSERT_EQUAL_UINT32(108u, s_published[0]);
    proto_srvq_ack(&q, 110u, 3u);
    TEST_ASSERT_EQUAL_UINT32(10u, q.acked);
}

/* The server is ahead of the saved position (the panel lost power before saving). */
static void test_queue_server_ahead(void)
{
    log_reset(10u);
    proto_srvq_t q;
    proto_srvq_init(&q, &PORT, 3u, 103u, true, 0u);
    proto_srvq_connected(&q, 1u);
    proto_srvq_time(&q, 108u, 2u);
    drain(&q, 2u);
    TEST_ASSERT_EQUAL_size_t(2u, s_pub_n);
    TEST_ASSERT_EQUAL_UINT32(109u, s_published[0]);
    TEST_ASSERT_EQUAL_UINT32(8u, q.acked);
}

/* The log wrapped while offline: it starts at the oldest record kept. */
static void test_queue_log_wrapped(void)
{
    log_reset(10u);
    s_oldest = 8u;
    proto_srvq_t q;
    proto_srvq_init(&q, &PORT, 2u, 102u, true, 0u);
    proto_srvq_connected(&q, 1u);
    proto_srvq_time(&q, 102u, 2u);
    drain(&q, 2u);
    TEST_ASSERT_EQUAL_size_t(3u, s_pub_n);
    TEST_ASSERT_EQUAL_UINT32(108u, s_published[0]);
}

static void test_queue_window_and_failed_publish(void)
{
    log_reset(40u);
    proto_srvq_t q;
    proto_srvq_init(&q, &PORT, 0u, 0u, true, 0u);
    proto_srvq_connected(&q, 1u);
    proto_srvq_time(&q, 0u, 2u);
    s_pub_ok = false;
    drain(&q, 2u);
    TEST_ASSERT_EQUAL_UINT32(0u, q.sent);
    s_pub_ok = true;
    drain(&q, 3u);
    TEST_ASSERT_EQUAL_size_t(PROTO_SRVQ_WINDOW, s_pub_n);
    proto_srvq_ack(&q, 104u, 4u);
    drain(&q, 4u);
    TEST_ASSERT_EQUAL_size_t(PROTO_SRVQ_WINDOW + 4u, s_pub_n);
    TEST_ASSERT_EQUAL_UINT32(4u, q.acked);
    /* An old ack changes nothing; one past everything in flight takes the whole window. */
    proto_srvq_ack(&q, 50u, 5u);
    TEST_ASSERT_EQUAL_UINT32(4u, q.acked);
    proto_srvq_ack(&q, 1000u, 5u);
    TEST_ASSERT_EQUAL_UINT32(4u + PROTO_SRVQ_WINDOW, q.acked);
}

/* MQTT-011's rule on this link. */
static void test_queue_comm_fail(void)
{
    log_reset(1u);
    proto_srvq_t q;
    proto_srvq_init(&q, &PORT, 0u, 0u, false, 0u);
    TEST_ASSERT_FALSE(proto_srvq_comm_fail(&q, PROTO_COMM_FAIL_MS * 2u));
    proto_srvq_enable(&q, true, 1000u);
    TEST_ASSERT_FALSE(proto_srvq_comm_fail(&q, PROTO_COMM_FAIL_MS));
    TEST_ASSERT_TRUE(proto_srvq_comm_fail(&q, 1000u + PROTO_COMM_FAIL_MS));
    proto_srvq_connected(&q, 2000u);
    proto_srvq_time(&q, 0u, 2000u);
    TEST_ASSERT_FALSE(proto_srvq_comm_fail(&q, 2000u + PROTO_COMM_FAIL_MS));
    drain(&q, 3000u); /* one event, never acknowledged */
    TEST_ASSERT_FALSE(proto_srvq_comm_fail(&q, 3000u + PROTO_COMM_FAIL_MS - 1u));
    TEST_ASSERT_TRUE(proto_srvq_comm_fail(&q, 3000u + PROTO_COMM_FAIL_MS));
    proto_srvq_ack(&q, 101u, 4000u);
    TEST_ASSERT_FALSE(proto_srvq_comm_fail(&q, 4000u + PROTO_COMM_FAIL_MS * 2u));
    /* Dropped a moment after the owner read its clock: not five minutes down. */
    proto_srvq_disconnected(&q, 5000u);
    TEST_ASSERT_FALSE(proto_srvq_comm_fail(&q, 4999u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_id_and_signed_bytes);
    RUN_TEST(test_golden_signin);
    RUN_TEST(test_base64url);
    RUN_TEST(test_password);
    RUN_TEST(test_ceiling);
    RUN_TEST(test_hello_golden);
    RUN_TEST(test_event_carries_clock);
    RUN_TEST(test_golden_result);
    RUN_TEST(test_golden_event);
    RUN_TEST(test_people_and_ceiling);
    RUN_TEST(test_small_messages);
    RUN_TEST(test_snapshot_has_panel_and_fits);
    RUN_TEST(test_parse_hostile);
    RUN_TEST(test_overflow_and_topics);
    RUN_TEST(test_refusal_names_and_clock_names);
    RUN_TEST(test_parse);
    RUN_TEST(test_command_map);
    RUN_TEST(test_sealed_code_is_bound);
    RUN_TEST(test_seal_signed_bytes);
    RUN_TEST(test_event_carries_its_actor);
    RUN_TEST(test_actor_is_parsed);
    RUN_TEST(test_sealed_code_for_a_slot);
    RUN_TEST(test_people_request_parse);
    RUN_TEST(test_command_run);
    RUN_TEST(test_req_rules);
    RUN_TEST(test_seen);
    RUN_TEST(test_clock);
    RUN_TEST(test_queue_replay_after_outage);
    RUN_TEST(test_queue_time_zero_keeps_position);
    RUN_TEST(test_queue_first_link_from_now);
    RUN_TEST(test_queue_server_lost_events);
    RUN_TEST(test_queue_server_ahead);
    RUN_TEST(test_queue_log_wrapped);
    RUN_TEST(test_queue_window_and_failed_publish);
    RUN_TEST(test_queue_comm_fail);
    RUN_TEST(test_delta_says_when);
    return UNITY_END();
}
