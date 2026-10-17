/*
 * test_proto.c - the panel's MQTT messages (components/core/alarm_proto): JSON writer and reader,
 * golden payloads, command parsing and Home Assistant discovery.
 * Requirements: MQTT-004, -005, -007, -008, -009, -010, -012, -013, -014, -015, -021, -022
 * (docs/features/10-MQTT-and-Home-Assistant); ZB-019, ZB-026 (docs/features/20 devices.md)
 */
#include <stdio.h>
#include <string.h>

#include "alarm/alarm_strings.h"
#include "alarm_proto/proto.h"
#include "alarm_proto/proto_fw.h"
#include "alarm_proto/proto_fwsrv.h"
#include "alarm_proto/proto_zb.h"
#include "notify/notify_auto.h"
#include "notify/notify_words.h"
#include "unity.h"

static alarm_config_t CFG;
static alarm_snapshot_t SNAP;
static char BUF[4096];
static pj_writer_t W;

static const proto_ident_t ID = {"alarmpanel", "a1b2c3", "0.5.0", "CrowPanel Advanced 7"};

void setUp(void)
{
    alarm_config_defaults(&CFG);
    snprintf(CFG.partitions[0].name, ALARM_NAME_LEN, "%s", "House");
    const uint8_t hash[ALARM_CODE_HASH_LEN] = {0};
    alarm_config_set_user(&CFG, 3, ALARM_AUTH_STANDARD, 0x01, "Alex", hash);
    alarm_zone_config_defaults(&CFG.zones[2], 3, ALARM_ZT_PERIMETER, 1);
    snprintf(CFG.zones[2].name, ALARM_NAME_LEN, "%s", "Living Window");
    CFG.zones[2].device_class = ALARM_DEVCLASS_WINDOW;
    alarm_zone_config_defaults(&CFG.zones[0], 1, ALARM_ZT_ENTRY_EXIT_1, 1);
    snprintf(CFG.zones[0].name, ALARM_NAME_LEN, "%s", "Front Door");
    CFG.zones[0].device_class = ALARM_DEVCLASS_DOOR;
    memset(&SNAP, 0, sizeof(SNAP));
    SNAP.partitions = 0x01;
    SNAP.taken_ms = 10000;
    pj_init(&W, BUF, sizeof(BUF));
}

void tearDown(void)
{
    notify_set_hooks(NULL);
}

static const char *done(void)
{
    TEST_ASSERT_TRUE_MESSAGE(pj_finish(&W) > 0u, "document overflowed or isn't closed");
    return BUF;
}

/* A document is valid JSON when the reader walks it to the end. */
static void skip_all(pj_reader_t *r)
{
    pj_skip(r);
}

static void starts_with(const char *prefix, const char *s)
{
    TEST_ASSERT_EQUAL_STRING_LEN(prefix, s, strlen(prefix));
}

static bool valid_json(const char *s)
{
    pj_reader_t r;
    pj_read_init(&r, s, strlen(s));
    skip_all(&r);
    return pj_finished(&r);
}

/* ---- JSON writer ----------------------------------------------------------------------------- */

/* MQTT-015: escaping, numbers, nesting; overflow never writes past the buffer. */
static void test_json_writer(void)
{
    pj_obj(&W);
    pj_kstr(&W, "q", "say \"hi\"\\ now\n\t\x01");
    pj_kstr(&W, "utf8", "K\xC3\xBC" "che \xE2\x9C\x93");
    pj_kstr(&W, "broken", "a\xFF" "b\xC3");
    pj_kint(&W, "min", INT64_MIN);
    pj_kuint(&W, "max", UINT64_MAX);
    pj_kint(&W, "neg", -42);
    pj_knum(&W, "none", 0);
    pj_knum(&W, "some", 7);
    pj_kstr(&W, "null", NULL);
    pj_karr(&W, "list");
    pj_bool(&W, true);
    pj_bool(&W, false);
    pj_obj(&W);
    pj_obj_end(&W);
    pj_arr(&W);
    pj_arr_end(&W);
    pj_arr_end(&W);
    pj_obj_end(&W);
    TEST_ASSERT_EQUAL_STRING(
        "{\"q\":\"say \\\"hi\\\"\\\\ now\\n\\t\\u0001\",\"utf8\":\"K\xC3\xBC" "che \xE2\x9C\x93\","
        "\"broken\":\"a\xEF\xBF\xBD" "b\xEF\xBF\xBD\",\"min\":-9223372036854775808,"
        "\"max\":18446744073709551615,\"neg\":-42,\"none\":null,\"some\":7,\"null\":null,"
        "\"list\":[true,false,{},[]]}",
        done());
    TEST_ASSERT_TRUE(valid_json(BUF));

    /* Overflow: stops cleanly, reports 0, keeps the NUL inside the buffer. */
    char small[16];
    memset(small, 'X', sizeof(small));
    pj_writer_t w;
    pj_init(&w, small, 12);
    pj_obj(&w);
    pj_kstr(&w, "key", "a long value");
    pj_obj_end(&w);
    TEST_ASSERT_EQUAL_UINT(0, pj_finish(&w));
    TEST_ASSERT_TRUE(strlen(small) < 12u);
    TEST_ASSERT_EQUAL_CHAR('X', small[12]);

    /* Unbalanced or misplaced keys are errors too. */
    pj_init(&w, small, sizeof(small));
    pj_obj(&w);
    TEST_ASSERT_EQUAL_UINT(0, pj_finish(&w));
    pj_init(&w, small, sizeof(small));
    pj_key(&w, "top");
    TEST_ASSERT_EQUAL_UINT(0, pj_finish(&w));
}

/* ---- JSON reader ----------------------------------------------------------------------------- */

/* MQTT-015: reads members of any order, decodes escapes, skips what isn't asked for. */
static void test_json_reader(void)
{
    static const char doc[] =
        " { \"a\" : \"x\\\"y\\u00e9\\ud83d\\ude00\\/\" , \"skip\" : {\"deep\":[1,2,{\"z\":null}]},"
        "\"n\":-12,\"f\":1.5e3,\"t\":true,\"arr\":[\"p\",\"q\"],\"nul\":null } ";
    pj_reader_t r;
    pj_read_init(&r, doc, strlen(doc));
    TEST_ASSERT_TRUE(pj_enter_obj(&r));
    char key[8];
    char s[32];
    int64_t n = 0;
    bool b = false;
    unsigned members = 0;
    unsigned elements = 0;
    while (pj_next_member(&r, key, sizeof(key))) {
        members++;
        if (strcmp(key, "a") == 0) {
            TEST_ASSERT_TRUE(pj_get_str(&r, s, sizeof(s)));
            TEST_ASSERT_EQUAL_STRING("x\"y\xC3\xA9\xF0\x9F\x98\x80/", s);
        } else if (strcmp(key, "n") == 0) {
            TEST_ASSERT_TRUE(pj_get_int(&r, &n));
            TEST_ASSERT_EQUAL_INT64(-12, n);
        } else if (strcmp(key, "f") == 0) {
            TEST_ASSERT_FALSE(pj_get_int(&r, &n)); /* not an integer: consumed, not an error */
        } else if (strcmp(key, "t") == 0) {
            TEST_ASSERT_TRUE(pj_get_bool(&r, &b));
            TEST_ASSERT_TRUE(b);
        } else if (strcmp(key, "arr") == 0) {
            TEST_ASSERT_TRUE(pj_enter_arr(&r));
            while (pj_next_element(&r)) {
                TEST_ASSERT_TRUE(pj_get_str(&r, s, sizeof(s)));
                elements++;
            }
        } else if (strcmp(key, "nul") == 0) {
            TEST_ASSERT_TRUE(pj_get_null(&r));
        }
        /* "skip" is never read: the next call skips it */
    }
    TEST_ASSERT_EQUAL_UINT(7, members);
    TEST_ASSERT_EQUAL_UINT(2, elements);
    TEST_ASSERT_TRUE(pj_finished(&r));

    /* Wrong types and strings that don't fit read as false without breaking the document. */
    static const char doc2[] = "{\"s\":5,\"long\":\"abcdefghij\",\"zero\":\"a\\u0000b\",\"x\":1}";
    pj_read_init(&r, doc2, strlen(doc2));
    TEST_ASSERT_TRUE(pj_enter_obj(&r));
    TEST_ASSERT_TRUE(pj_next_member(&r, key, sizeof(key)));
    TEST_ASSERT_FALSE(pj_get_str(&r, s, sizeof(s)));
    TEST_ASSERT_TRUE(pj_next_member(&r, key, sizeof(key)));
    TEST_ASSERT_FALSE(pj_get_str(&r, s, 5));
    TEST_ASSERT_EQUAL_STRING("", s);
    TEST_ASSERT_TRUE(pj_next_member(&r, key, sizeof(key)));
    TEST_ASSERT_FALSE(pj_get_str(&r, s, sizeof(s)));
    TEST_ASSERT_TRUE(pj_next_member(&r, key, sizeof(key)));
    TEST_ASSERT_FALSE(pj_next_member(&r, key, sizeof(key)));
    TEST_ASSERT_TRUE(pj_finished(&r));
}

/* MQTT-015: malformed or hostile input is rejected; nesting is bounded. */
static void test_json_reader_rejects(void)
{
    static const char *const bad[] = {
        "",
        "{",
        "{\"a\":1,}",
        "{\"a\" 1}",
        "{\"a\":01}",
        "{\"a\":-}",
        "{\"a\":1.}",
        "{\"a\":\"raw\ttab\"}",
        "{\"a\":\"\\ud800\"}",
        "{\"a\":\"\\udc00\"}",
        "{\"a\":\"\\x41\"}",
        "{\"a\":\"open}",
        "{\"a\":tru}",
        "{\"a\":1} extra",
        "{\"a\":[1 2]}",
        "{a:1}",
        "[[[[[[[[[1]]]]]]]]]",
        "{\"a\":{\"b\":{\"c\":{\"d\":{\"e\":{\"f\":{\"g\":{\"h\":{}}}}}}}}}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(valid_json(bad[i]), bad[i]);
    }
    TEST_ASSERT_TRUE(valid_json("[[[[[[[[1]]]]]]]]"));
    TEST_ASSERT_TRUE(valid_json(" {\"a\":[-0,0.5e-3,1E+2,\"\\b\\f\\n\\r\\t\"]} "));
}

/* ---- identity and topics --------------------------------------------------------------------- */

/* MQTT-013: prefix and panel id rules, the id from the MAC, topic layout. */
static void test_topics_and_ids(void)
{
    const uint8_t mac[6] = {0x24, 0x58, 0x7C, 0xA1, 0xB2, 0xC3};
    char id[PROTO_PANEL_ID_LEN];
    proto_panel_id_from_mac(mac, id);
    TEST_ASSERT_EQUAL_STRING("a1b2c3", id);
    TEST_ASSERT_TRUE(proto_panel_id_valid(id));
    TEST_ASSERT_FALSE(proto_panel_id_valid(""));
    TEST_ASSERT_FALSE(proto_panel_id_valid("A1B2C3"));
    TEST_ASSERT_FALSE(proto_panel_id_valid("a/b"));
    TEST_ASSERT_FALSE(proto_panel_id_valid("0123456789abcdefg"));

    TEST_ASSERT_TRUE(proto_prefix_valid(PROTO_PREFIX_DEFAULT));
    TEST_ASSERT_TRUE(proto_prefix_valid("home/alarm"));
    TEST_ASSERT_FALSE(proto_prefix_valid("home/+"));
    TEST_ASSERT_FALSE(proto_prefix_valid("#"));
    TEST_ASSERT_FALSE(proto_prefix_valid("/home"));
    TEST_ASSERT_FALSE(proto_prefix_valid("home/"));
    TEST_ASSERT_FALSE(proto_prefix_valid("home//alarm"));
    TEST_ASSERT_FALSE(proto_prefix_valid("has space"));
    TEST_ASSERT_FALSE(proto_prefix_valid("0123456789012345678901234567890123"));

    char t[PROTO_TOPIC_LEN];
    TEST_ASSERT_TRUE(proto_topic(&ID, "/availability", t, sizeof(t)));
    TEST_ASSERT_EQUAL_STRING("alarmpanel/a1b2c3/availability", t);
    TEST_ASSERT_TRUE(proto_topic_partition(&ID, 2, "set", t, sizeof(t)));
    TEST_ASSERT_EQUAL_STRING("alarmpanel/a1b2c3/partition/2/set", t);
    TEST_ASSERT_TRUE(proto_topic_zone(&ID, 128, t, sizeof(t)));
    TEST_ASSERT_EQUAL_STRING("alarmpanel/a1b2c3/zone/128/state", t);
    TEST_ASSERT_FALSE(proto_topic(&ID, "/availability", t, 10));

    proto_ident_t nested = ID;
    snprintf(nested.prefix, sizeof(nested.prefix), "%s", "home/alarm");
    TEST_ASSERT_TRUE(proto_ha_topic(&nested, "binary_sensor", "z3", t, sizeof(t)));
    TEST_ASSERT_EQUAL_STRING("homeassistant/binary_sensor/home_alarm_a1b2c3/z3/config", t);
}

/* ---- state ----------------------------------------------------------------------------------- */

/* The mapping table in docs/features/10; MQTT-010: silent alarms and duress never trigger. */
static void test_ha_state_mapping(void)
{
    alarm_partition_status_t ps;
    memset(&ps, 0, sizeof(ps));
    TEST_ASSERT_EQUAL_STRING("disarmed", proto_ha_state(&ps));
    ps.state = ALARM_STATE_EXIT_DELAY;
    ps.level = ALARM_LEVEL_AWAY;
    TEST_ASSERT_EQUAL_STRING("arming", proto_ha_state(&ps));
    ps.state = ALARM_STATE_ARMED;
    TEST_ASSERT_EQUAL_STRING("armed_away", proto_ha_state(&ps));
    ps.level = ALARM_LEVEL_STAY;
    TEST_ASSERT_EQUAL_STRING("armed_home", proto_ha_state(&ps));
    ps.level = ALARM_LEVEL_NIGHT;
    TEST_ASSERT_EQUAL_STRING("armed_night", proto_ha_state(&ps));
    ps.state = ALARM_STATE_ENTRY_DELAY;
    TEST_ASSERT_EQUAL_STRING("pending", proto_ha_state(&ps));
    ps.memory = (uint16_t)(1u << ALARM_CAUSE_PANIC_SILENT); /* silent: triggered stays false */
    ps.state = ALARM_STATE_ARMED;
    TEST_ASSERT_EQUAL_STRING("armed_night", proto_ha_state(&ps));
    ps.triggered = true;
    TEST_ASSERT_EQUAL_STRING("triggered", proto_ha_state(&ps));
    ps.state = ALARM_STATE_DISARMED; /* e.g. a fire alarm while disarmed */
    TEST_ASSERT_EQUAL_STRING("triggered", proto_ha_state(&ps));
}

static void armed_away_snapshot(void)
{
    alarm_partition_status_t *ps = &SNAP.part[0];
    ps->state = ALARM_STATE_ARMED;
    ps->level = ALARM_LEVEL_AWAY;
    ps->armed_by = 3;
    ps->ready_levels = (uint8_t)(ALARM_LEVEL_BIT(ALARM_LEVEL_STAY) | ALARM_LEVEL_BIT(ALARM_LEVEL_NIGHT));
    ps->memory = (uint16_t)(1u << ALARM_CAUSE_BURGLARY);
    ps->faults = 1;
    ps->alarms = 1;
    ps->triggered = true;
    ps->sounder = ALARM_SOUND_ALARM;
}

/* MQTT-004: the retained partition status, with and without the fields that tick. */
static void test_partition_status_golden(void)
{
    armed_away_snapshot();
    proto_partition_status(&W, &CFG, &SNAP, 1, NULL);
    TEST_ASSERT_EQUAL_STRING(
        "{\"partition\":1,\"name\":\"House\",\"state\":\"armed\",\"ha_state\":\"triggered\","
        "\"level\":\"away\",\"no_entry_delay\":false,\"silent_exit\":false,\"ready\":false,"
        "\"ready_levels\":[\"stay\",\"night\"],\"triggered\":true,\"exit_error\":false,"
        "\"chime\":false,\"walk_test\":false,\"walk_test_tested\":0,"
        "\"walk_test_total\":0,\"installer_mode\":false,"
        "\"armed_by\":{\"user\":3,\"name\":\"Alex\"},\"memory\":[\"burglary\"],"
        "\"memory_canceled\":false,\"faults\":1,\"bypassed\":0,\"alarms\":1,\"troubles\":0,"
        "\"trouble_beeps\":false,\"sounder\":\"alarm\"}",
        done());

    /* Exit time: the countdown is taken at publication; ts is the wall clock. */
    memset(&SNAP.part[0], 0, sizeof(SNAP.part[0]));
    SNAP.part[0].state = ALARM_STATE_EXIT_DELAY;
    SNAP.part[0].level = ALARM_LEVEL_STAY;
    SNAP.part[0].arm_flags = ALARM_ARM_SILENT_EXIT;
    SNAP.part[0].delay_remaining_ms = 60000;
    SNAP.part[0].ready = true;
    SNAP.part[0].ready_levels = 0x0E;
    const proto_when_t when = {SNAP.taken_ms + 15500u, 1789857660000LL};
    pj_init(&W, BUF, sizeof(BUF));
    proto_partition_status(&W, &CFG, &SNAP, 1, &when);
    TEST_ASSERT_EQUAL_STRING(
        "{\"partition\":1,\"name\":\"House\",\"state\":\"exit_delay\",\"ha_state\":\"arming\","
        "\"level\":\"stay\",\"no_entry_delay\":false,\"silent_exit\":true,\"ready\":true,"
        "\"ready_levels\":[\"away\",\"stay\",\"night\"],\"delay_remaining_s\":45,"
        "\"triggered\":false,\"exit_error\":false,\"chime\":false,\"walk_test\":false,"
        "\"walk_test_tested\":0,\"walk_test_total\":0,\"walk_test_remaining_s\":0,"
        "\"installer_mode\":false,\"armed_by\":null,\"memory\":[],\"memory_canceled\":false,"
        "\"faults\":0,\"bypassed\":0,\"alarms\":0,\"troubles\":0,\"trouble_beeps\":false,"
        "\"sounder\":\"none\",\"ts\":\"2026-09-19T22:41:00Z\"}",
        done());

    /* WALK-019: a walk test adds its counts and the time left, counted down like the delay. */
    memset(&SNAP.part[0], 0, sizeof(SNAP.part[0]));
    SNAP.part[0].state = ALARM_STATE_DISARMED;
    SNAP.part[0].ready = true;
    SNAP.part[0].walk_test = true;
    SNAP.part[0].walk_tested = 14;
    SNAP.part[0].walk_total = 16;
    SNAP.part[0].walk_test_remaining_ms = 600000;
    const proto_when_t later = {SNAP.taken_ms + 30500u, 1789857660000LL};
    pj_init(&W, BUF, sizeof(BUF));
    proto_partition_status(&W, &CFG, &SNAP, 1, &later);
    TEST_ASSERT_NOT_NULL(strstr(done(),
        "\"walk_test\":true,\"walk_test_tested\":14,\"walk_test_total\":16,"
        "\"walk_test_remaining_s\":570,\"installer_mode\""));
}

/* MQTT-004: zone state and troubles. */
static void test_zone_and_troubles_golden(void)
{
    SNAP.zone[2].flags = ALARM_ZS_FAULT | ALARM_ZS_NOT_READY | ALARM_ZS_LOW_BATTERY;
    SNAP.zone[2].trouble = (uint8_t)(1u << ALARM_ZTRBL_LOW_BATTERY);
    proto_zone_state(&W, &CFG, &SNAP, 3);
    TEST_ASSERT_EQUAL_STRING(
        "{\"zone\":3,\"name\":\"Living Window\",\"type\":\"perimeter\",\"device_class\":\"window\","
        "\"partition\":1,\"open\":true,\"bypassed\":false,\"alarm\":false,\"trouble\":false,"
        "\"tamper\":false,\"low_battery\":true,\"supervision\":false,\"not_ready\":true}",
        done());

    /* docs/features/37 I18N-009, I18N-033: a name the household typed in any script, as long
     * as a name may be, goes out as the UTF-8 it is. */
    snprintf(CFG.zones[2].name, ALARM_NAME_LEN, "%s",
             "Sal\xC3\xB3n de la planta baja \xC2\xB7 K\xC3\xBC" "che");
    pj_init(&W, BUF, sizeof(BUF));
    proto_zone_state(&W, &CFG, &SNAP, 3);
    TEST_ASSERT_NOT_NULL(strstr(done(), "\"name\":\"Sal\xC3\xB3n de la planta baja \xC2\xB7 K\xC3\xBC"
                                        "che\""));
    snprintf(CFG.zones[2].name, ALARM_NAME_LEN, "%s", "Living Window");

    SNAP.sys_troubles = 1u << ALARM_TRBL_AC_LOSS;
    SNAP.sys_troubles_restored = 1u << ALARM_TRBL_NETWORK;
    SNAP.dev_troubles = 1u << 1;
    SNAP.part[0].trouble_unack = true;
    snprintf(CFG.devices[1].name, ALARM_NAME_LEN, "%s", "Hall Keypad");
    pj_init(&W, BUF, sizeof(BUF));
    proto_troubles(&W, &CFG, &SNAP);
    TEST_ASSERT_EQUAL_STRING(
        "{\"active\":true,\"count\":3,\"trouble_beeps\":true,\"system\":[\"ac_loss\"],"
        "\"system_restored\":[\"network\"],\"zones\":[{\"zone\":3,\"name\":\"Living Window\","
        "\"troubles\":[\"low_battery\"]}],\"zones_more\":0,"
        "\"devices\":[{\"device\":2,\"name\":\"Hall Keypad\"}]}",
        done());

    /* Nothing active: the Home Assistant sensor reads off; restored ones are still listed. */
    SNAP.sys_troubles = 0;
    SNAP.dev_troubles = 0;
    SNAP.zone[2].trouble = 0;
    SNAP.part[0].trouble_unack = false;
    pj_init(&W, BUF, sizeof(BUF));
    proto_troubles(&W, &CFG, &SNAP);
    TEST_ASSERT_EQUAL_STRING(
        "{\"active\":false,\"count\":0,\"trouble_beeps\":false,\"system\":[],"
        "\"system_restored\":[\"network\"],\"zones\":[],\"zones_more\":0,\"devices\":[]}",
        done());
}

/* ---- events ---------------------------------------------------------------------------------- */

static alarm_event_t event(alarm_event_type_t type)
{
    alarm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.seq = 1523;
    ev.epoch_ms = 1789857660000LL;
    ev.type = type;
    ev.partition = 1;
    ev.flags = ALARM_EVF_REPORT;
    return ev;
}

/* MQTT-005: the event payload (Home Assistant's event entity reads event_type). */
static void test_event_golden(void)
{
    alarm_event_t ev = event(ALARM_EV_ALARM);
    ev.zone = 3;
    ev.source.kind = ALARM_SRC_ZONE;
    ev.source.id = 3;
    ev.arg = ALARM_CAUSE_BURGLARY;
    proto_event(&W, &CFG, &ev, false);
    TEST_ASSERT_EQUAL_STRING(
        "{\"seq\":1523,\"ts\":\"2026-09-19T22:41:00Z\",\"event_type\":\"alarm\",\"partition\":1,"
        "\"zone\":3,\"zone_name\":\"Living Window\",\"user\":null,\"user_name\":null,"
        "\"source\":\"zone\",\"source_id\":3,\"cause\":\"burglary\",\"silent\":false,"
        "\"replayed\":false}",
        done());

    /* Armed Stay by Alex from Home Assistant, sent again after a reconnect, clock unknown. */
    ev = event(ALARM_EV_ARMED);
    ev.epoch_ms = 0;
    ev.user = 3;
    ev.source.kind = ALARM_SRC_REMOTE;
    ev.source.id = 1;
    ev.arg = ALARM_LEVEL_STAY;
    ev.arg2 = ALARM_ARM_NO_ENTRY_DELAY;
    pj_init(&W, BUF, sizeof(BUF));
    proto_event(&W, &CFG, &ev, true);
    TEST_ASSERT_EQUAL_STRING(
        "{\"seq\":1523,\"ts\":null,\"event_type\":\"armed\",\"partition\":1,\"zone\":null,"
        "\"zone_name\":null,\"user\":3,\"user_name\":\"Alex\",\"source\":\"remote\","
        "\"source_id\":1,\"level\":\"stay\",\"no_entry_delay\":true,\"silent_exit\":false,"
        "\"silent\":false,\"replayed\":true}",
        done());

    /* System and device troubles. */
    ev = event(ALARM_EV_TROUBLE);
    ev.partition = 0;
    ev.arg = ALARM_TRBL_COMM_FAIL;
    ev.arg2 = ALARM_TRBL_FAMILY_SYSTEM;
    pj_init(&W, BUF, sizeof(BUF));
    proto_event(&W, &CFG, &ev, false);
    TEST_ASSERT_NOT_NULL(strstr(done(), "\"family\":\"system\",\"trouble\":\"comm_fail\""));
    TEST_ASSERT_NOT_NULL(strstr(BUF, "\"partition\":null"));

    snprintf(CFG.devices[1].name, ALARM_NAME_LEN, "%s", "Hall Keypad");
    ev = event(ALARM_EV_TROUBLE_RESTORE);
    ev.zone = 2;
    ev.arg = ALARM_TRBL_DEVICE_SUPERVISION;
    ev.arg2 = ALARM_TRBL_FAMILY_DEVICE;
    pj_init(&W, BUF, sizeof(BUF));
    proto_event(&W, &CFG, &ev, false);
    TEST_ASSERT_NOT_NULL(strstr(done(), "\"zone\":null,\"zone_name\":null"));
    TEST_ASSERT_NOT_NULL(strstr(BUF, "\"family\":\"device\",\"trouble\":\"supervision\","
                                     "\"device\":2,\"device_name\":\"Hall Keypad\""));
}

/* MQTT-010: duress and silent panics are published, marked silent. */
static void test_silent_events_are_marked(void)
{
    alarm_event_t ev = event(ALARM_EV_DURESS);
    ev.user = 3;
    ev.flags = ALARM_EVF_REPORT | ALARM_EVF_SILENT;
    proto_event(&W, &CFG, &ev, false);
    TEST_ASSERT_NOT_NULL(strstr(done(), "\"event_type\":\"duress\""));
    TEST_ASSERT_NOT_NULL(strstr(BUF, "\"silent\":true"));

    ev = event(ALARM_EV_ALARM);
    ev.arg = ALARM_CAUSE_PANIC_SILENT;
    ev.flags = ALARM_EVF_REPORT | ALARM_EVF_SILENT;
    pj_init(&W, BUF, sizeof(BUF));
    proto_event(&W, &CFG, &ev, false);
    TEST_ASSERT_NOT_NULL(strstr(done(), "\"cause\":\"panic_silent\",\"silent\":true"));
}

/* MQTT-012: heartbeat. */
static void test_heartbeat_golden(void)
{
    const proto_heartbeat_t hb = {93784, "0.5.0", "192.168.1.20", -58, 412, 7, 0};
    proto_heartbeat(&W, &hb);
    TEST_ASSERT_EQUAL_STRING("{\"uptime_s\":93784,\"fw\":\"0.5.0\",\"ip\":\"192.168.1.20\","
                             "\"rssi\":-58,\"free_heap_kb\":412,\"boot\":7,\"unsent\":0}",
                             done());
    const proto_heartbeat_t offline = {5, "0.5.0", "", 0, 400, 8, 3};
    pj_init(&W, BUF, sizeof(BUF));
    proto_heartbeat(&W, &offline);
    TEST_ASSERT_NOT_NULL(strstr(done(), "\"ip\":null,\"rssi\":null"));
}

/* Leap days and the limits of the time stamp. */
static void test_iso8601(void)
{
    char ts[24];
    TEST_ASSERT_TRUE(proto_iso8601(951782400000LL, ts));
    TEST_ASSERT_EQUAL_STRING("2000-02-29T00:00:00Z", ts);
    TEST_ASSERT_TRUE(proto_iso8601(1709208000999LL, ts));
    TEST_ASSERT_EQUAL_STRING("2024-02-29T12:00:00Z", ts);
    TEST_ASSERT_TRUE(proto_iso8601(1, ts));
    TEST_ASSERT_EQUAL_STRING("1970-01-01T00:00:00Z", ts);
    TEST_ASSERT_FALSE(proto_iso8601(0, ts));
    TEST_ASSERT_EQUAL_STRING("", ts);
    TEST_ASSERT_FALSE(proto_iso8601(-5, ts));
    TEST_ASSERT_FALSE(proto_iso8601(253402300800000LL, ts));
}

/* ---- commands -------------------------------------------------------------------------------- */

static alarm_result_t cmd(const char *json, bool allow_panic, proto_cmd_t *out)
{
    return proto_cmd_parse(json, strlen(json), allow_panic, out);
}

/* MQTT-007, MQTT-008: the command JSON becomes an engine command; the core checks the code. */
static void test_command_parse(void)
{
    proto_cmd_t c;
    TEST_ASSERT_EQUAL_INT(ALARM_OK,
                          cmd("{\"id\":\"8f2c\",\"action\":\"arm\",\"level\":\"away\",\"partition\":2,"
                              "\"code\":\"246810\",\"flags\":[\"silent_exit\",\"no_entry_delay\"],"
                              "\"future\":{\"x\":1}}",
                              false, &c));
    TEST_ASSERT_EQUAL_STRING("8f2c", c.id);
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_ARM, c.cmd.type);
    TEST_ASSERT_EQUAL_INT(ALARM_LEVEL_AWAY, c.cmd.level);
    TEST_ASSERT_EQUAL_UINT8(2, c.cmd.partition);
    TEST_ASSERT_EQUAL_STRING("246810", c.cmd.code);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ARM_SILENT_EXIT | ALARM_ARM_NO_ENTRY_DELAY, c.cmd.arm_flags);

    TEST_ASSERT_EQUAL_INT(ALARM_OK, cmd("{\"action\":\"arm\",\"level\":\"home\",\"code\":null}", false, &c));
    TEST_ASSERT_EQUAL_INT(ALARM_LEVEL_STAY, c.cmd.level);
    TEST_ASSERT_EQUAL_UINT8(1, c.cmd.partition);
    TEST_ASSERT_EQUAL_STRING("", c.cmd.code); /* no code: the core decides (MQTT-008) */
    TEST_ASSERT_EQUAL_STRING("", c.id);

    TEST_ASSERT_EQUAL_INT(ALARM_OK, cmd("{\"action\":\"disarm\",\"code\":\"1234\"}", false, &c));
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_DISARM, c.cmd.type);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, cmd("{\"action\":\"bypass\",\"zone\":3,\"code\":\"1234\"}", false, &c));
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_BYPASS, c.cmd.type);
    TEST_ASSERT_EQUAL_UINT16(3, c.cmd.zone);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, cmd("{\"action\":\"unbypass\",\"zone\":3}", false, &c));
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_UNBYPASS, c.cmd.type);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, cmd("{\"action\":\"chime\",\"code\":\"1234\"}", false, &c));
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_CHIME, c.cmd.type);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, cmd("{\"action\":\"exit_restart\"}", false, &c));
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_EXIT_RESTART, c.cmd.type);

    /* Panic only when turned on. */
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_DISABLED, cmd("{\"id\":\"p\",\"action\":\"panic\",\"panic\":\"fire\"}", false, &c));
    TEST_ASSERT_EQUAL_STRING("p", c.id);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, cmd("{\"action\":\"panic\",\"panic\":\"medical\"}", true, &c));
    TEST_ASSERT_EQUAL_INT(ALARM_PANIC_MEDICAL, c.cmd.panic);

    /* Rejected: the id still comes back for the result. */
    static const char *const bad[] = {
        "not json",
        "{\"action\":\"arm\"}",
        "{\"action\":\"arm\",\"level\":\"vacation\"}",
        "{\"action\":\"bypass\"}",
        "{\"action\":\"bypass\",\"zone\":0}",
        "{\"action\":\"bypass\",\"zone\":999999}",
        "{\"action\":\"disarm\",\"code\":\"12a4\"}",
        "{\"action\":\"disarm\",\"code\":\"12345678901\"}",
        "{\"action\":\"disarm\",\"code\":1234}",
        "{\"action\":\"disarm\",\"partition\":9}",
        "{\"action\":\"disarm\",\"flags\":[\"silent_exit\"]}",
        "{\"action\":\"arm\",\"level\":\"away\",\"flags\":[\"loud\"]}",
        "{\"action\":\"arm\",\"level\":\"away\",\"flags\":\"silent_exit\"}",
        "{\"action\":\"panic\",\"panic\":\"loud\"}",
        "{\"action\":\"reboot\"}",
        "{\"action\":\"disarm\",\"code\":\"1234\"",
        "[\"disarm\"]",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(ALARM_ERR_BAD_ARG, cmd(bad[i], true, &c), bad[i]);
    }
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_BAD_ARG,
                          cmd("{\"id\":\"q1\",\"action\":\"disarm\",\"code\":\"x\"}", false, &c));
    TEST_ASSERT_EQUAL_STRING("q1", c.id);
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_BAD_ARG,
                          cmd("{\"id\":\"0123456789012345678901234567890123456789X\",\"action\":\"disarm\"}", false, &c));

    /* Result JSON. */
    TEST_ASSERT_EQUAL_INT(ALARM_OK, cmd("{\"id\":\"8f2c\",\"action\":\"arm\",\"level\":\"away\"}", false, &c));
    const alarm_cmd_result_t res = {.user = 0, .not_ready_zone = 3};
    proto_cmd_result(&W, &c, ALARM_ERR_NOT_READY, &res);
    TEST_ASSERT_EQUAL_STRING("{\"id\":\"8f2c\",\"action\":\"arm\",\"result\":\"not_ready\","
                             "\"user\":null,\"zone\":3}",
                             done());
    pj_init(&W, BUF, sizeof(BUF));
    proto_cmd_result(&W, NULL, ALARM_ERR_BAD_ARG, NULL);
    TEST_ASSERT_EQUAL_STRING("{\"id\":null,\"action\":null,\"result\":\"bad_arg\"}", done());
}

static alarm_result_t ha(const char *payload, bool allow_trigger, alarm_cmd_t *out)
{
    return proto_ha_cmd_parse(payload, strlen(payload), 2, allow_trigger, out);
}

/* MQTT-007, MQTT-008: Home Assistant's alarm panel commands. */
static void test_ha_command_parse(void)
{
    alarm_cmd_t c;
    TEST_ASSERT_EQUAL_INT(ALARM_OK, ha("{\"action\": \"ARM_AWAY\", \"code\": \"246810\"}", false, &c));
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_ARM, c.type);
    TEST_ASSERT_EQUAL_INT(ALARM_LEVEL_AWAY, c.level);
    TEST_ASSERT_EQUAL_UINT8(2, c.partition);
    TEST_ASSERT_EQUAL_STRING("246810", c.code);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, ha("{\"action\":\"ARM_HOME\",\"code\":null}", false, &c));
    TEST_ASSERT_EQUAL_INT(ALARM_LEVEL_STAY, c.level);
    TEST_ASSERT_EQUAL_STRING("", c.code);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, ha("{\"action\":\"ARM_NIGHT\"}", false, &c));
    TEST_ASSERT_EQUAL_INT(ALARM_LEVEL_NIGHT, c.level);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, ha(" DISARM\n", false, &c));
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_DISARM, c.type);
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_DISABLED, ha("{\"action\":\"TRIGGER\"}", false, &c));
    TEST_ASSERT_EQUAL_INT(ALARM_OK, ha("TRIGGER", true, &c));
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_PANIC, c.type);
    TEST_ASSERT_EQUAL_INT(ALARM_PANIC_POLICE, c.panic);
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_BAD_ARG, ha("ARM_VACATION", true, &c));
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_BAD_ARG, ha("ARM_CUSTOM_BYPASS", true, &c));
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_BAD_ARG, ha("{\"action\":\"DISARM\",\"code\":\"12;4\"}", true, &c));
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_BAD_ARG, ha("{\"action\":\"DISARM\"", true, &c));
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_BAD_ARG, ha("", true, &c));
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_BAD_ARG, ha("DISARM_EVERYTHING_RIGHT_NOW_PLEASE", true, &c));
}

/* ---- discovery ------------------------------------------------------------------------------- */

/* MQTT-009, MQTT-014: discovery documents are valid JSON, fit the publish buffer, and follow
 * the options (read-only, arming without a code, trigger). */
static void test_discovery(void)
{
    char t[PROTO_TOPIC_LEN];
    TEST_ASSERT_TRUE(proto_ha_topic(&ID, "alarm_control_panel", "p1", t, sizeof(t)));
    TEST_ASSERT_EQUAL_STRING("homeassistant/alarm_control_panel/alarmpanel_a1b2c3/p1/config", t);

    const proto_ha_opts_t full = {true, false, false};
    proto_ha_panel(&W, &ID, &CFG, 1, &full);
    const char *doc = done();
    TEST_ASSERT_TRUE(valid_json(doc));
    TEST_ASSERT_TRUE(strlen(doc) < 2048u); /* small enough to keep retained */
    starts_with("{\"name\":null,\"unique_id\":\"alarmpanel_a1b2c3_p1\","
                "\"~\":\"alarmpanel/a1b2c3\",\"availability_topic\":\"~/availability\","
                "\"device\":{\"identifiers\":[\"alarmpanel_a1b2c3\"],\"name\":\"Alarm Panel\","
                "\"manufacturer\":\"RoboAlarms Panel (open source)\",\"model\":\"CrowPanel Advanced 7\","
                "\"sw_version\":\"0.5.0\"},\"origin\":{\"name\":\"RoboAlarms Panel\",\"sw_version\":\"0.5.0\"},",
                doc);
    TEST_ASSERT_NOT_NULL(strstr(doc, "\"state_topic\":\"~/partition/1/state\""));
    TEST_ASSERT_NOT_NULL(strstr(doc, "\"command_topic\":\"~/partition/1/set\""));
    TEST_ASSERT_NOT_NULL(strstr(doc, "\"json_attributes_topic\":\"~/partition/1/status\""));
    TEST_ASSERT_NOT_NULL(strstr(doc, "\"command_template\":\"{{ {\\\"action\\\": action, "
                                     "\\\"code\\\": code} | to_json }}\""));
    TEST_ASSERT_NOT_NULL(strstr(doc, "\"code\":\"REMOTE_CODE\",\"code_arm_required\":true,"
                                     "\"code_disarm_required\":true"));
    TEST_ASSERT_NOT_NULL(strstr(doc, "\"supported_features\":[\"arm_home\",\"arm_away\",\"arm_night\"]"));
    TEST_ASSERT_NOT_NULL(strstr(doc, "\"sw_version\":\"0.5.0\""));

    const proto_ha_opts_t easy = {true, true, true};
    pj_init(&W, BUF, sizeof(BUF));
    proto_ha_panel(&W, &ID, &CFG, 1, &easy);
    TEST_ASSERT_NOT_NULL(strstr(done(), "\"code_arm_required\":false"));
    TEST_ASSERT_NOT_NULL(strstr(BUF, "\"arm_night\",\"trigger\"]"));

    const proto_ha_opts_t read_only = {false, false, true};
    pj_init(&W, BUF, sizeof(BUF));
    proto_ha_panel(&W, &ID, &CFG, 1, &read_only);
    TEST_ASSERT_NOT_NULL(strstr(done(), "\"supported_features\":[]"));

    /* Two partitions: each panel is named after its partition. */
    CFG.partitions[1].enabled = true;
    snprintf(CFG.partitions[1].name, ALARM_NAME_LEN, "%s", "Garage");
    pj_init(&W, BUF, sizeof(BUF));
    proto_ha_panel(&W, &ID, &CFG, 2, &full);
    starts_with("{\"name\":\"Garage\",\"unique_id\":\"alarmpanel_a1b2c3_p2\"", done());

    pj_init(&W, BUF, sizeof(BUF));
    proto_ha_zone(&W, &ID, &CFG, 1);
    doc = done();
    TEST_ASSERT_TRUE(valid_json(doc));
    TEST_ASSERT_NOT_NULL(strstr(doc, "{\"name\":\"Front Door\",\"unique_id\":\"alarmpanel_a1b2c3_z1\""));
    TEST_ASSERT_NOT_NULL(strstr(doc, "\"state_topic\":\"~/zone/1/state\","
                                     "\"json_attributes_topic\":\"~/zone/1/state\","
                                     "\"value_template\":\"{{ 'ON' if value_json.open else 'OFF' }}\","
                                     "\"device_class\":\"door\"}"));
    CFG.zones[4].type = ALARM_ZT_24H_AUX; /* unnamed generic zone */
    pj_init(&W, BUF, sizeof(BUF));
    proto_ha_zone(&W, &ID, &CFG, 5);
    TEST_ASSERT_NOT_NULL(strstr(done(), "{\"name\":\"Zone 5\""));
    TEST_ASSERT_NULL(strstr(BUF, "device_class"));

    pj_init(&W, BUF, sizeof(BUF));
    proto_ha_trouble(&W, &ID);
    TEST_ASSERT_TRUE(valid_json(done()));
    TEST_ASSERT_NOT_NULL(strstr(BUF, "\"device_class\":\"problem\",\"entity_category\":\"diagnostic\""));

    pj_init(&W, BUF, sizeof(BUF));
    proto_ha_event(&W, &ID);
    doc = done();
    TEST_ASSERT_TRUE(valid_json(doc));
    TEST_ASSERT_TRUE(strlen(doc) < 2048u); /* small enough to keep retained */
    for (unsigned e = 0; e < ALARM_EV_COUNT; e++) {
        char quoted[40];
        snprintf(quoted, sizeof(quoted), "\"%s\"", alarm_event_name((alarm_event_type_t)e));
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(doc, quoted), quoted);
    }

    TEST_ASSERT_EQUAL_STRING("moisture", proto_ha_device_class(ALARM_DEVCLASS_WATER));
    TEST_ASSERT_EQUAL_STRING("carbon_monoxide", proto_ha_device_class(ALARM_DEVCLASS_CO));
    TEST_ASSERT_NULL(proto_ha_device_class(ALARM_DEVCLASS_KEYFOB));
}

/* ---- the Zigbee network ---------------------------------------------------------------------- */

/* MQTT-021, ZB-026 (docs/features/20 devices.md §15): the network's retained state and its
 * diagnostic sensor in Home Assistant. Golden payloads, written out by hand from the document. */
static void test_zigbee_state_golden(void)
{
    char t[PROTO_TOPIC_LEN];
    TEST_ASSERT_TRUE(proto_topic(&ID, PROTO_ZB_STATE_SUFFIX, t, sizeof(t)));
    TEST_ASSERT_EQUAL_STRING("alarmpanel/a1b2c3/zigbee/state", t);
    TEST_ASSERT_TRUE(proto_ha_topic(&ID, "sensor", PROTO_ZB_OBJECT, t, sizeof(t)));
    TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/alarmpanel_a1b2c3/zigbee/config", t);

    const proto_zb_state_t working = {PROTO_ZB_RADIO_WORKING, 15u, 5u, 0u, false};
    proto_zb_state(&W, &working);
    TEST_ASSERT_EQUAL_STRING(
        "{\"radio\":\"working\",\"channel\":15,\"devices\":5,\"not_heard\":0,\"jammed\":false}",
        done());

    /* No network yet: the channel is null, never a number nobody can use. */
    const proto_zb_state_t starting = {PROTO_ZB_RADIO_STARTING, 0u, 0u, 0u, false};
    pj_init(&W, BUF, sizeof(BUF));
    proto_zb_state(&W, &starting);
    TEST_ASSERT_EQUAL_STRING(
        "{\"radio\":\"starting\",\"channel\":null,\"devices\":0,\"not_heard\":0,\"jammed\":false}",
        done());
    const proto_zb_state_t odd = {200u, 27u, 64u, 3u, true}; /* out of range: words, not junk */
    pj_init(&W, BUF, sizeof(BUF));
    proto_zb_state(&W, &odd);
    TEST_ASSERT_EQUAL_STRING(
        "{\"radio\":\"unknown\",\"channel\":null,\"devices\":64,\"not_heard\":3,\"jammed\":true}",
        done());

    /* The radio's words are a contract (the numbers are ui_zb_radio_t's). */
    static const char *const RADIO[] = {"not_connected", "starting",     "working", "not_answering",
                                        "paused",        "socket_zwave", "unusable"};
    TEST_ASSERT_EQUAL_UINT(PROTO_ZB_RADIO_COUNT, sizeof(RADIO) / sizeof(RADIO[0]));
    for (uint8_t r = 0; r < PROTO_ZB_RADIO_COUNT; r++) {
        TEST_ASSERT_EQUAL_STRING(RADIO[r], proto_zb_radio_name(r));
    }
    TEST_ASSERT_EQUAL_STRING("unknown", proto_zb_radio_name(PROTO_ZB_RADIO_COUNT));

    pj_init(&W, BUF, sizeof(BUF));
    proto_ha_zb(&W, &ID);
    const char *doc = done();
    TEST_ASSERT_TRUE(valid_json(doc));
    TEST_ASSERT_EQUAL_STRING(
        "{\"name\":\"Zigbee\",\"unique_id\":\"alarmpanel_a1b2c3_zigbee\","
        "\"~\":\"alarmpanel/a1b2c3\",\"availability_topic\":\"~/availability\","
        "\"device\":{\"identifiers\":[\"alarmpanel_a1b2c3\"],\"name\":\"Alarm Panel\","
        "\"manufacturer\":\"RoboAlarms Panel (open source)\",\"model\":\"CrowPanel Advanced 7\","
        "\"sw_version\":\"0.5.0\"},\"origin\":{\"name\":\"RoboAlarms Panel\",\"sw_version\":\"0.5.0\"},"
        "\"state_topic\":\"~/zigbee/state\",\"json_attributes_topic\":\"~/zigbee/state\","
        "\"value_template\":\"{{ value_json.radio }}\",\"icon\":\"mdi:zigbee\","
        "\"entity_category\":\"diagnostic\"}",
        doc);
}

/* MQTT-022, ZB-026: a zone fed by a Zigbee device carries its battery and signal as attributes
 * of the zone's own entity, after the keys every zone has; without them the zone state is
 * byte for byte what it was (the golden of test_zone_and_troubles_golden). */
static void test_zone_diag_golden(void)
{
    static const char OLD[] =
        "{\"zone\":3,\"name\":\"Living Window\",\"type\":\"perimeter\",\"device_class\":\"window\","
        "\"partition\":1,\"open\":true,\"bypassed\":false,\"alarm\":false,\"trouble\":false,"
        "\"tamper\":false,\"low_battery\":false,\"supervision\":false,\"not_ready\":true}";
    SNAP.zone[2].flags = ALARM_ZS_FAULT | ALARM_ZS_NOT_READY;

    proto_zone_state_diag(&W, &CFG, &SNAP, 3, NULL);
    TEST_ASSERT_EQUAL_STRING(OLD, done());
    const proto_zone_diag_t unknown = {PROTO_BATTERY_UNKNOWN, PROTO_SIGNAL_UNKNOWN};
    pj_init(&W, BUF, sizeof(BUF));
    proto_zone_state_diag(&W, &CFG, &SNAP, 3, &unknown);
    TEST_ASSERT_EQUAL_STRING(OLD, done());
    const proto_zone_diag_t junk = {101u, PROTO_SIGNAL_COUNT}; /* out of range: left out */
    pj_init(&W, BUF, sizeof(BUF));
    proto_zone_state_diag(&W, &CFG, &SNAP, 3, &junk);
    TEST_ASSERT_EQUAL_STRING(OLD, done());

    const proto_zone_diag_t fair = {87u, PROTO_SIGNAL_FAIR};
    pj_init(&W, BUF, sizeof(BUF));
    proto_zone_state_diag(&W, &CFG, &SNAP, 3, &fair);
    TEST_ASSERT_EQUAL_STRING(
        "{\"zone\":3,\"name\":\"Living Window\",\"type\":\"perimeter\",\"device_class\":\"window\","
        "\"partition\":1,\"open\":true,\"bypassed\":false,\"alarm\":false,\"trouble\":false,"
        "\"tamper\":false,\"low_battery\":false,\"supervision\":false,\"not_ready\":true,"
        "\"battery\":87,\"signal\":\"fair\"}",
        done());
    const proto_zone_diag_t empty_weak = {0u, PROTO_SIGNAL_WEAK};
    pj_init(&W, BUF, sizeof(BUF));
    proto_zone_state_diag(&W, &CFG, &SNAP, 3, &empty_weak);
    TEST_ASSERT_NOT_NULL(strstr(done(), "\"not_ready\":true,\"battery\":0,\"signal\":\"weak\"}"));
    const proto_zone_diag_t signal_only = {PROTO_BATTERY_UNKNOWN, PROTO_SIGNAL_GOOD};
    pj_init(&W, BUF, sizeof(BUF));
    proto_zone_state_diag(&W, &CFG, &SNAP, 3, &signal_only);
    TEST_ASSERT_NOT_NULL(strstr(done(), "\"not_ready\":true,\"signal\":\"good\"}"));
    TEST_ASSERT_NULL(strstr(BUF, "\"battery\""));

    /* The zone's entity reads them from its existing attributes topic: no new entity. */
    pj_init(&W, BUF, sizeof(BUF));
    proto_ha_zone(&W, &ID, &CFG, 3);
    TEST_ASSERT_NOT_NULL(strstr(done(), "\"json_attributes_topic\":\"~/zone/3/state\""));

    /* A zone out of range writes nothing usable. */
    pj_init(&W, BUF, sizeof(BUF));
    proto_zone_state_diag(&W, &CFG, &SNAP, 0, &fair);
    TEST_ASSERT_EQUAL_UINT(0u, pj_finish(&W));
}

/* MQTT-005, ZB-019: the Zigbee network's events go out on ~/event like the others, by name. */
static void test_zigbee_event_golden(void)
{
    alarm_event_t ev = event(ALARM_EV_ZIGBEE_LEFT);
    ev.partition = 0;
    ev.zone = 3;
    ev.arg = 9;
    ev.source.kind = ALARM_SRC_SYSTEM;
    ev.flags = ALARM_EVF_REPORT | ALARM_EVF_SYSTEM;
    proto_event(&W, &CFG, &ev, false);
    TEST_ASSERT_EQUAL_STRING(
        "{\"seq\":1523,\"ts\":\"2026-09-19T22:41:00Z\",\"event_type\":\"zigbee_left\","
        "\"partition\":null,\"zone\":3,\"zone_name\":\"Living Window\",\"user\":null,"
        "\"user_name\":null,\"source\":\"system\",\"source_id\":0,\"silent\":false,"
        "\"replayed\":false}",
        done());
    static const char *const NAMES[] = {
        "zigbee_added",   "zigbee_removed",      "zigbee_forgotten",
        "zigbee_left",    "zigbee_replaced",     "zigbee_join_refused",
        "zigbee_channel", "zigbee_network_reset", "zigbee_radio_restarted"};
    for (unsigned i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        ev = event((alarm_event_type_t)(ALARM_EV_ZIGBEE_ADDED + i));
        pj_init(&W, BUF, sizeof(BUF));
        proto_event(&W, &CFG, &ev, false);
        char want[64];
        snprintf(want, sizeof(want), "\"event_type\":\"%s\"", NAMES[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(done(), want), want);
    }
    /* and Home Assistant's event entity lists them, still small enough to keep retained */
    pj_init(&W, BUF, sizeof(BUF));
    proto_ha_event(&W, &ID);
    const char *doc = done();
    TEST_ASSERT_TRUE(strlen(doc) < 2048u);
    TEST_ASSERT_NOT_NULL(strstr(doc, "\"zigbee_radio_restarted\",\"automation_failed\",\"arming_put_off\"]"));
}

/* MQTT-005, AUT-009 (docs/features/41 section 7): an automation that didn't lock, close or arm
 * goes out on ~/event with the automation's uid (source_id), its words (null here: the test
 * registers no hook), the action and the reason, by name. */
static void test_automation_failed_event_golden(void)
{
    alarm_event_t ev = event(ALARM_EV_AUTOMATION_FAILED);
    ev.partition = 0;
    ev.zone = 3;
    ev.user = 3;
    ev.source.kind = ALARM_SRC_AUTOMATION;
    ev.source.id = 12;
    ev.arg = ALARM_AUTO_FAIL_NO_ANSWER;
    ev.arg2 = ALARM_AUTO_ACT_LOCK;
    ev.flags = ALARM_EVF_REPORT | ALARM_EVF_SYSTEM;
    proto_event(&W, &CFG, &ev, false);
    TEST_ASSERT_EQUAL_STRING(
        "{\"seq\":1523,\"ts\":\"2026-09-19T22:41:00Z\",\"event_type\":\"automation_failed\","
        "\"partition\":null,\"zone\":3,\"zone_name\":\"Living Window\",\"user\":3,"
        "\"user_name\":\"Alex\",\"source\":\"automation\",\"source_id\":12,"
        "\"action\":\"lock\",\"reason\":\"no_answer\",\"automation\":null,\"saved_by\":\"Alex\","
        "\"silent\":false,\"replayed\":false}",
        done());
    TEST_ASSERT_TRUE(strlen(BUF) < PROTO_PAYLOAD_MAX);

    /* C's arming with no time to warn first: no zone, arg2 0. */
    ev.zone = 0;
    ev.user = 0;
    ev.source.id = 4;
    ev.arg = ALARM_AUTO_FAIL_NO_WARNING;
    ev.arg2 = ALARM_AUTO_ACT_ARM;
    pj_init(&W, BUF, sizeof(BUF));
    proto_event(&W, &CFG, &ev, true);
    TEST_ASSERT_EQUAL_STRING(
        "{\"seq\":1523,\"ts\":\"2026-09-19T22:41:00Z\",\"event_type\":\"automation_failed\","
        "\"partition\":null,\"zone\":null,\"zone_name\":null,\"user\":null,"
        "\"user_name\":null,\"source\":\"automation\",\"source_id\":4,"
        "\"action\":\"arm\",\"reason\":\"no_warning\",\"automation\":null,\"saved_by\":null,"
        "\"silent\":false,\"replayed\":true}",
        done());

    /* Every reason and action has its word; anything else reads "unknown", never a gap. */
    static const char *const REASONS[] = {"unreachable", "no_answer", "refused", "missing",
                                          "busy", "not_allowed", "no_warning"};
    for (unsigned r = 1; r < (unsigned)ALARM_AUTO_FAIL_COUNT; r++) {
        TEST_ASSERT_EQUAL_STRING(REASONS[r - 1u], alarm_auto_fail_name((alarm_auto_fail_t)r));
    }
    TEST_ASSERT_EQUAL_STRING("unknown", alarm_auto_fail_name((alarm_auto_fail_t)0));
    TEST_ASSERT_EQUAL_STRING("unknown", alarm_auto_fail_name(ALARM_AUTO_FAIL_COUNT));
    TEST_ASSERT_EQUAL_STRING("close", alarm_auto_action_name(ALARM_AUTO_ACT_CLOSE));
    TEST_ASSERT_EQUAL_STRING("unknown", alarm_auto_action_name(1u));

    /* Home Assistant's event entity lists it, still small enough to keep retained. */
    pj_init(&W, BUF, sizeof(BUF));
    proto_ha_event(&W, &ID);
    const char *doc = done();
    printf("event_types discovery document: %u bytes\n", (unsigned)strlen(doc));
    TEST_ASSERT_TRUE(strlen(doc) < 2048u);
    TEST_ASSERT_NOT_NULL(strstr(doc, "\"automation_failed\""));
}

/* MQTT-005, OTA-010, docs/features/11 "Updates through the link" (What people see, the event
 * log): a keypad's update stages go out on ~/event by name, the keypad as source "keypad" and
 * source_id; keypad_failed adds its why, by number and by the why table's name ("unknown" for one
 * this firmware doesn't know: a reader takes it by its decade, LINK-039). Keys are added only. */
static void test_keypad_firmware_event_golden(void)
{
    alarm_event_t ev = event(ALARM_EV_FIRMWARE_UPDATE);
    ev.partition = 0;
    ev.source.kind = ALARM_SRC_KEYPAD;
    ev.source.id = 2;
    ev.arg = ALARM_FW_KEYPAD_FAILED;
    ev.arg2 = PROTO_FW_WHY_STALLED;
    ev.flags = ALARM_EVF_REPORT | ALARM_EVF_SYSTEM;
    proto_event(&W, &CFG, &ev, false);
    TEST_ASSERT_EQUAL_STRING(
        "{\"seq\":1523,\"ts\":\"2026-09-19T22:41:00Z\",\"event_type\":\"firmware_update\","
        "\"partition\":null,\"zone\":null,\"zone_name\":null,\"user\":null,"
        "\"user_name\":null,\"source\":\"keypad\",\"source_id\":2,"
        "\"stage\":\"keypad_failed\",\"why\":24,\"reason\":\"stalled\","
        "\"silent\":false,\"replayed\":false}",
        done());

    /* the other stages carry no why */
    ev.arg = ALARM_FW_KEYPAD_KEY_ACCEPTED;
    ev.arg2 = 0;
    ev.user = 1;
    pj_init(&W, BUF, sizeof(BUF));
    proto_event(&W, &CFG, &ev, false);
    const char *doc = done();
    TEST_ASSERT_NOT_NULL(strstr(doc, "\"source\":\"keypad\",\"source_id\":2,"
                                     "\"stage\":\"keypad_key_accepted\",\"silent\":false"));
    TEST_ASSERT_NULL(strstr(doc, "\"why\""));

    /* every why has its name, the why table's in lower case */
    static const struct {
        unsigned why;
        const char *name;
    } NAMES[] = {
        {PROTO_FWSRV_WHY_NO_NETWORK, "no_network"},
        {PROTO_FWSRV_WHY_NO_LOOKUP, "no_lookup"},
        {PROTO_FWSRV_WHY_NO_CONNECT, "no_connect"},
        {PROTO_FWSRV_WHY_SERVER_ERROR, "server_error"},
        {PROTO_FWSRV_WHY_NOT_A_SERVER, "not_a_server"},
        {PROTO_FWSRV_WHY_UNKNOWN_DEVICE, "unknown_device"},
        {PROTO_FWSRV_WHY_HELD_BACK, "held_back"},
        {PROTO_FWSRV_WHY_STEPPING_STONE, "stepping_stone"},
        {PROTO_FWSRV_WHY_OTHER_VERSION, "other_version"},
        {PROTO_FWSRV_WHY_UP_TO_DATE, "up_to_date"},
        {PROTO_FWSRV_WHY_UNKNOWN_VERSION, "unknown_version"},
        {PROTO_FWSRV_WHY_NOT_OFFERED, "not_offered"},
        {PROTO_FWSRV_WHY_REFUSED, "refused"},
        {PROTO_FW_WHY_BROKE_OFF, "broke_off"},
        {PROTO_FW_WHY_TOO_SLOW, "too_slow"},
        {PROTO_FW_WHY_BAD_SIZE, "bad_size"},
        {PROTO_FW_WHY_BAD_SHA, "bad_sha"},
        {PROTO_FW_WHY_STALLED, "stalled"},
        {PROTO_FW_WHY_TIMEOUT, "timeout"},
        {PROTO_FW_WHY_NO_MEMORY, "no_memory"},
        {PROTO_FW_WHY_FLASH, "flash"},
        {PROTO_FW_WHY_KEY, "key"},
        {PROTO_FW_WHY_CHIP, "chip"},
        {PROTO_FW_WHY_PROJECT, "project"},
        {PROTO_FW_WHY_NOT_NEWER, "not_newer"},
        {PROTO_FW_WHY_SIGNATURE, "signature"},
        {PROTO_FW_WHY_TOO_BIG, "too_big"},
        {PROTO_FW_WHY_UNKNOWN_KIND, "unknown_kind"},
        {PROTO_FW_WHY_UNKNOWN_OP, "unknown_op"},
        {PROTO_FW_WHY_BUDGET, "budget"},
        {PROTO_FW_WHY_BAD_ORDER, "bad_order"},
        {PROTO_FW_WHY_WRONG_VERSION, "wrong_version"},
        {PROTO_FW_WHY_MASTER_BUSY, "master_busy"},
        {PROTO_FW_WHY_NOT_QUIET, "not_quiet"},
        {PROTO_FW_WHY_PANIC_PENDING, "panic_pending"},
        {PROTO_FW_WHY_KEPT_TONE, "kept_tone"},
        {PROTO_FW_WHY_NO_STATUS, "no_status"},
        {PROTO_FW_WHY_BUSY, "busy"},
        {PROTO_FW_WHY_STOPPED, "stopped"},
        {PROTO_FW_WHY_CANCELLED, "cancelled"},
    };
    unsigned named = 0;
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        TEST_ASSERT_EQUAL_STRING(NAMES[i].name, proto_fw_why_name(NAMES[i].why));
    }
    for (unsigned why = 0; why < 256u; why++) {
        named += strcmp(proto_fw_why_name(why), "unknown") != 0 ? 1u : 0u;
    }
    TEST_ASSERT_EQUAL_UINT(sizeof(NAMES) / sizeof(NAMES[0]), named);
    TEST_ASSERT_EQUAL_STRING("unknown", proto_fw_why_name(PROTO_FW_WHY_OK));
    TEST_ASSERT_EQUAL_STRING("unknown", proto_fw_why_name(39u));
    TEST_ASSERT_EQUAL_STRING("unknown", proto_fw_why_name(77u));

    /* notify spells the why numbers out (alarm_proto requires notify, not the other way round):
     * its words follow these numbers, each its own reason's (CANCELLED reads "Stopped", as its
     * decade and the master's row do) */
    static const struct {
        unsigned why;
        const char *line;
    } WORDS[] = {
        {PROTO_FWSRV_WHY_UNKNOWN_DEVICE, "The update server has nothing for it"},
        {PROTO_FWSRV_WHY_HELD_BACK, "The update server holds it back for now"},
        {PROTO_FWSRV_WHY_STEPPING_STONE, "It needs another version first"},
        {PROTO_FWSRV_WHY_UNKNOWN_VERSION,
         "The update server has no release of this version for it"},
        {PROTO_FWSRV_WHY_NOT_OFFERED, "The update server doesn't offer this version"},
        {PROTO_FW_WHY_BROKE_OFF, "The download broke off"},
        {PROTO_FW_WHY_TOO_SLOW, "The download was too slow"},
        {PROTO_FW_WHY_BAD_SIZE, "It got the wrong size"},
        {PROTO_FW_WHY_BAD_SHA, "It arrived damaged"},
        {PROTO_FW_WHY_STALLED, "No data for a minute"},
        {PROTO_FW_WHY_TIMEOUT, "It waited too long"},
        {PROTO_FW_WHY_NO_MEMORY, "Not enough memory"},
        {PROTO_FW_WHY_FLASH, "It couldn't store it"},
        {PROTO_FW_WHY_KEY, "The new signing key must be accepted at the keypad"},
        {PROTO_FW_WHY_CHIP, "It isn't for this keypad's chip"},
        {PROTO_FW_WHY_PROJECT, "It isn't from a project it accepts"},
        {PROTO_FW_WHY_NOT_NEWER, "It isn't newer than the version it runs"},
        {PROTO_FW_WHY_SIGNATURE, "The signature check failed"},
        {PROTO_FW_WHY_TOO_BIG, "It's too big for this keypad"},
        {PROTO_FW_WHY_UNKNOWN_KIND, "It can't take this kind of update"},
        {PROTO_FW_WHY_UNKNOWN_OP, "It doesn't know this order"},
        {PROTO_FW_WHY_BUDGET, "3 tries failed, next try tomorrow"},
        {PROTO_FW_WHY_BAD_ORDER, "It refused the order"},
        {PROTO_FW_WHY_WRONG_VERSION, "It got another version"},
        {PROTO_FW_WHY_MASTER_BUSY, "The system is busy"},
        {PROTO_FW_WHY_NOT_QUIET, "It waits until everything is quiet"},
        {PROTO_FW_WHY_PANIC_PENDING, "A panic is being sent from it"},
        {PROTO_FW_WHY_KEPT_TONE, "Its fire or CO alarm still sounds"},
        {PROTO_FW_WHY_NO_STATUS, "Its link is down"},
        {PROTO_FW_WHY_BUSY, "Another update runs on it"},
        {PROTO_FW_WHY_STOPPED, "Stopped at the keypad"},
        {PROTO_FW_WHY_CANCELLED, "Stopped"},
    };
    ev.arg = ALARM_FW_KEYPAD_FAILED;
    for (size_t i = 0; i < sizeof(WORDS) / sizeof(WORDS[0]); i++) {
        notify_words_t w;
        ev.arg2 = (uint8_t)WORDS[i].why;
        TEST_ASSERT_TRUE(notify_words(NULL, &CFG, &ev, true, &w));
        TEST_ASSERT_EQUAL_STRING("Keypad 2 update failed", w.title);
        TEST_ASSERT_EQUAL_STRING(WORDS[i].line, w.body);
    }
}

/* Every payload for a full panel fits PROTO_PAYLOAD_MAX (the publish buffer in net_mqtt). */
/* The longest words a hook may give (D30): a full clause and a full sentence. */
static bool longest_lookup(void *ctx, const i18n_catalog_t *c, const alarm_config_t *conf,
                           uint16_t uid, notify_auto_info_t *out)
{
    (void)ctx;
    (void)c;
    (void)conf;
    (void)uid;
    memset(out->clause, 'C', sizeof(out->clause) - 1u);
    memset(out->sentence, 'S', sizeof(out->sentence) - 1u);
    out->maker = ALARM_MAX_USERS;
    return true;
}

static void test_payloads_fit(void)
{
    for (unsigned z = 1; z <= ALARM_MAX_ZONES; z++) {
        alarm_zone_config_defaults(&CFG.zones[z - 1u], (uint16_t)z, ALARM_ZT_PERIMETER, 1);
        memset(CFG.zones[z - 1u].name, 'W', ALARM_NAME_LEN - 1u);
        SNAP.zone[z - 1u].trouble = 0x1F;
    }
    for (unsigned u = 1; u <= ALARM_MAX_USERS; u++) {
        memset(CFG.users[u - 1u].name, 'U', ALARM_NAME_LEN - 1u);
    }
    memset(CFG.partitions[0].name, 'P', ALARM_NAME_LEN - 1u);
    SNAP.sys_troubles = 0xFFFFFFFFu;
    SNAP.sys_troubles_restored = 0xFFFFFFFFu;
    SNAP.dev_troubles = 0xFFFFFFFFu;
    SNAP.part[0].memory = 0xFFFFu;
    SNAP.part[0].ready_levels = 0x0E;
    SNAP.part[0].armed_by = ALARM_MAX_USERS;
    const proto_when_t when = {0, 1789857660000LL};
    char big[PROTO_PAYLOAD_MAX];
    pj_writer_t w;
    pj_init(&w, big, sizeof(big));
    proto_partition_status(&w, &CFG, &SNAP, 1, &when);
    TEST_ASSERT_TRUE(pj_finish(&w) > 0u);
    pj_init(&w, big, sizeof(big));
    proto_troubles(&w, &CFG, &SNAP);
    TEST_ASSERT_TRUE(pj_finish(&w) > 0u);
    TEST_ASSERT_NOT_NULL(strstr(big, "\"zones_more\":112"));
    pj_init(&w, big, sizeof(big));
    proto_zone_state(&w, &CFG, &SNAP, ALARM_MAX_ZONES);
    TEST_ASSERT_TRUE(pj_finish(&w) > 0u);
    const proto_zone_diag_t diag = {100u, PROTO_SIGNAL_WEAK};
    pj_init(&w, big, sizeof(big));
    proto_zone_state_diag(&w, &CFG, &SNAP, ALARM_MAX_ZONES, &diag);
    TEST_ASSERT_TRUE(pj_finish(&w) > 0u);
    const proto_zb_state_t zb = {PROTO_ZB_RADIO_NOT_ANSWERING, 26u, 65535u, 65535u, true};
    pj_init(&w, big, sizeof(big));
    proto_zb_state(&w, &zb);
    TEST_ASSERT_TRUE(pj_finish(&w) > 0u);
    alarm_event_t ev = event(ALARM_EV_USER_UPDATED);
    ev.user = ALARM_MAX_USERS;
    ev.arg = ALARM_MAX_USERS;
    ev.zone = ALARM_MAX_ZONES;
    pj_init(&w, big, sizeof(big));
    proto_event(&w, &CFG, &ev, true);
    TEST_ASSERT_TRUE(pj_finish(&w) > 0u);
    /* MQTT-004/005, D30: an automation's longest words, in the state's armed_by and in an
     * event, still fit. */
    const notify_hooks_t longest = {longest_lookup, NULL, NULL};
    notify_set_hooks(&longest);
    SNAP.part[0].arm_source.kind = ALARM_SRC_AUTOMATION;
    SNAP.part[0].arm_source.id = 65535u;
    pj_init(&w, big, sizeof(big));
    proto_partition_status(&w, &CFG, &SNAP, 1, &when);
    TEST_ASSERT_TRUE(pj_finish(&w) > 0u);
    TEST_ASSERT_NOT_NULL(strstr(big, "SSSS"));
    ev = event(ALARM_EV_AUTOMATION_FAILED);
    ev.user = ALARM_MAX_USERS;
    ev.zone = ALARM_MAX_ZONES;
    ev.source.kind = ALARM_SRC_AUTOMATION;
    ev.source.id = 65535u;
    ev.arg = ALARM_AUTO_FAIL_NOT_ALLOWED;
    ev.arg2 = ALARM_AUTO_ACT_CLOSE;
    pj_init(&w, big, sizeof(big));
    proto_event(&w, &CFG, &ev, true);
    TEST_ASSERT_TRUE(pj_finish(&w) > 0u);
    TEST_ASSERT_NOT_NULL(strstr(big, "SSSS"));
    notify_set_hooks(NULL);
    SNAP.part[0].arm_source.kind = ALARM_SRC_KEYPAD;
    SNAP.part[0].arm_source.id = 0u;
    char t_zb[PROTO_TOPIC_LEN];
    proto_ident_t id = ID;
    memset(id.prefix, 'p', PROTO_PREFIX_LEN - 1u);
    memset(id.panel_id, 'i', PROTO_PANEL_ID_LEN - 1u);
    const proto_ha_opts_t opts = {true, false, true};
    pj_init(&w, big, sizeof(big));
    proto_ha_panel(&w, &id, &CFG, 1, &opts);
    TEST_ASSERT_TRUE(pj_finish(&w) > 0u);
    pj_init(&w, big, sizeof(big));
    proto_ha_event(&w, &id);
    TEST_ASSERT_TRUE(pj_finish(&w) > 0u);
    pj_init(&w, big, sizeof(big));
    proto_ha_zb(&w, &id);
    TEST_ASSERT_TRUE(pj_finish(&w) > 0u);
    TEST_ASSERT_TRUE(proto_topic(&id, PROTO_ZB_STATE_SUFFIX, t_zb, sizeof(t_zb)));
    TEST_ASSERT_TRUE(proto_ha_topic(&id, "sensor", PROTO_ZB_OBJECT, t_zb, sizeof(t_zb)));
    char t[PROTO_TOPIC_LEN];
    TEST_ASSERT_TRUE(proto_ha_topic(&id, "alarm_control_panel", "p1", t, sizeof(t)));
    TEST_ASSERT_TRUE(proto_topic_partition(&id, 1, "status", t, sizeof(t)));
}

/* ---- docs/features/41 D30: who did it ------------------------------------------------- */

/* The test's automations: uid 12 "At 10:00 AM on weekdays", saved by Alex (user 3). */
static bool fake_lookup(void *ctx, const i18n_catalog_t *c, const alarm_config_t *conf,
                        uint16_t uid, notify_auto_info_t *out)
{
    (void)ctx;
    if (uid != 12u) {
        return false;
    }
    auto_trigger_t w;
    memset(&w, 0, sizeof(w));
    w.kind = AUTO_TRIG_TIME;
    w.minute = 600u;
    w.days = 0x1Fu;
    notify_auto_when(c, conf, NULL, false, &w, out->clause, sizeof(out->clause));
    out->changed_at = 1u;
    out->maker = 3u;
    return true;
}

static void use_fake_lookup(void)
{
    const notify_hooks_t h = {fake_lookup, NULL, NULL};
    notify_set_hooks(&h);
}

/* MQTT-004, AUT-009, D30: the state's armed_by says an automation armed it ("source" first, which
 * one), keeping user and name (who saved it) for older readers; a person's is unchanged. */
static void test_armed_by_automation_golden(void)
{
    use_fake_lookup();
    armed_away_snapshot();
    SNAP.part[0].arm_source.kind = ALARM_SRC_AUTOMATION;
    SNAP.part[0].arm_source.id = 12u;
    proto_partition_status(&W, &CFG, &SNAP, 1, NULL);
    TEST_ASSERT_NOT_NULL(strstr(done(), "\"armed_by\":{\"user\":3,\"name\":\"Alex\","
                                        "\"source\":\"automation\",\"source_id\":12,"
                                        "\"automation\":\"At 10:00 AM on weekdays\"},"));
    TEST_ASSERT_TRUE(strlen(BUF) < PROTO_PAYLOAD_MAX);
    /* After a restart (uid 0) or once deleted: still the automation, naming none. */
    SNAP.part[0].arm_source.id = 0u;
    pj_init(&W, BUF, sizeof(BUF));
    proto_partition_status(&W, &CFG, &SNAP, 1, NULL);
    TEST_ASSERT_NOT_NULL(strstr(done(), "\"armed_by\":{\"user\":3,\"name\":\"Alex\","
                                        "\"source\":\"automation\",\"source_id\":null,"
                                        "\"automation\":null},"));
    SNAP.part[0].arm_source.id = 99u;
    pj_init(&W, BUF, sizeof(BUF));
    proto_partition_status(&W, &CFG, &SNAP, 1, NULL);
    TEST_ASSERT_NOT_NULL(strstr(done(), "\"source_id\":99,\"automation\":null},"));
    /* A person's arming: byte for byte as before. */
    SNAP.part[0].arm_source.kind = ALARM_SRC_KEYPAD;
    pj_init(&W, BUF, sizeof(BUF));
    proto_partition_status(&W, &CFG, &SNAP, 1, NULL);
    TEST_ASSERT_NOT_NULL(strstr(done(), "\"armed_by\":{\"user\":3,\"name\":\"Alex\"},"));
}

/* MQTT-005, AUT-009, D30: an automation's event names it (its words) and who saved it;
 * user/user_name stay for older readers. Deleted, or without a hook: automation null. */
static void test_automation_source_event_golden(void)
{
    use_fake_lookup();
    alarm_event_t ev = event(ALARM_EV_DISARMED);
    ev.user = 3;
    ev.source.kind = ALARM_SRC_AUTOMATION;
    ev.source.id = 12;
    ev.flags = ALARM_EVF_REPORT;
    proto_event(&W, &CFG, &ev, false);
    TEST_ASSERT_EQUAL_STRING(
        "{\"seq\":1523,\"ts\":\"2026-09-19T22:41:00Z\",\"event_type\":\"disarmed\","
        "\"partition\":1,\"zone\":null,\"zone_name\":null,\"user\":3,\"user_name\":\"Alex\","
        "\"source\":\"automation\",\"source_id\":12,\"automation\":\"At 10:00 AM on weekdays\","
        "\"saved_by\":\"Alex\",\"silent\":false,\"replayed\":false}",
        done());
    ev.source.id = 13; /* deleted since */
    pj_init(&W, BUF, sizeof(BUF));
    proto_event(&W, &CFG, &ev, false);
    TEST_ASSERT_NOT_NULL(strstr(done(), "\"source_id\":13,\"automation\":null,"
                                        "\"saved_by\":\"Alex\",\"silent\""));
    notify_set_hooks(NULL);
    ev.type = ALARM_EV_ARMED;
    ev.arg = ALARM_LEVEL_NIGHT;
    ev.arg2 = (uint8_t)(1u << ALARM_ARMED_BYPASSED_SHIFT);
    ev.source.id = 12;
    pj_init(&W, BUF, sizeof(BUF));
    proto_event(&W, &CFG, &ev, false);
    TEST_ASSERT_EQUAL_STRING(
        "{\"seq\":1523,\"ts\":\"2026-09-19T22:41:00Z\",\"event_type\":\"armed\","
        "\"partition\":1,\"zone\":null,\"zone_name\":null,\"user\":3,\"user_name\":\"Alex\","
        "\"source\":\"automation\",\"source_id\":12,\"level\":\"night\","
        "\"no_entry_delay\":false,\"silent_exit\":false,\"automation\":null,"
        "\"saved_by\":\"Alex\",\"silent\":false,\"replayed\":false}",
        done());
    /* A person's event: no new keys. */
    ev.source.kind = ALARM_SRC_KEYPAD;
    ev.source.id = 0;
    pj_init(&W, BUF, sizeof(BUF));
    proto_event(&W, &CFG, &ev, false);
    TEST_ASSERT_NULL(strstr(done(), "saved_by"));
    TEST_ASSERT_NULL(strstr(BUF, "\"automation\""));
}

/* MQTT-005, AUT-013, D26: a put-off goes out with whose code (user), the automation, the choice
 * and until when (epoch ms; null for a skip or without a clock). */
static void test_put_off_event_golden(void)
{
    use_fake_lookup();
    snprintf(CFG.users[3].name, ALARM_NAME_LEN, "%s", "Pat");
    alarm_event_t ev = event(ALARM_EV_ARMING_PUT_OFF);
    ev.user = 4;
    ev.source.kind = ALARM_SRC_AUTOMATION;
    ev.source.id = 12;
    ev.arg = ALARM_PUT_OFF_HOUR;
    ev.arg2 = alarm_put_off_minutes(ev.epoch_ms, ev.epoch_ms + 64 * 60000);
    ev.flags = ALARM_EVF_REPORT;
    proto_event(&W, &CFG, &ev, false);
    TEST_ASSERT_EQUAL_STRING(
        "{\"seq\":1523,\"ts\":\"2026-09-19T22:41:00Z\",\"event_type\":\"arming_put_off\","
        "\"partition\":1,\"zone\":null,\"zone_name\":null,\"user\":4,\"user_name\":\"Pat\","
        "\"source\":\"automation\",\"source_id\":12,\"choice\":\"hour\","
        "\"until\":1789861500000,\"automation\":\"At 10:00 AM on weekdays\","
        "\"saved_by\":\"Alex\",\"silent\":false,\"replayed\":false}",
        done());
    TEST_ASSERT_TRUE(strlen(BUF) < PROTO_PAYLOAD_MAX);
    ev.arg = ALARM_PUT_OFF_NIGHT;
    ev.arg2 = 0u;
    ev.source.id = 13; /* deleted since: no maker known, never the person who put it off */
    pj_init(&W, BUF, sizeof(BUF));
    proto_event(&W, &CFG, &ev, false);
    TEST_ASSERT_NOT_NULL(strstr(done(), "\"user\":4,\"user_name\":\"Pat\","));
    TEST_ASSERT_NOT_NULL(strstr(BUF, "\"choice\":\"night\",\"until\":null,\"automation\":null,"
                                     "\"saved_by\":null,"));
    ev.arg = ALARM_PUT_OFF_DAY;
    pj_init(&W, BUF, sizeof(BUF));
    proto_event(&W, &CFG, &ev, false);
    TEST_ASSERT_NOT_NULL(strstr(done(), "\"choice\":\"day\""));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_json_writer);
    RUN_TEST(test_json_reader);
    RUN_TEST(test_json_reader_rejects);
    RUN_TEST(test_topics_and_ids);
    RUN_TEST(test_ha_state_mapping);
    RUN_TEST(test_partition_status_golden);
    RUN_TEST(test_zone_and_troubles_golden);
    RUN_TEST(test_event_golden);
    RUN_TEST(test_silent_events_are_marked);
    RUN_TEST(test_heartbeat_golden);
    RUN_TEST(test_iso8601);
    RUN_TEST(test_command_parse);
    RUN_TEST(test_ha_command_parse);
    RUN_TEST(test_discovery);
    RUN_TEST(test_zigbee_state_golden);
    RUN_TEST(test_zone_diag_golden);
    RUN_TEST(test_zigbee_event_golden);
    RUN_TEST(test_automation_failed_event_golden);
    RUN_TEST(test_keypad_firmware_event_golden);
    RUN_TEST(test_armed_by_automation_golden);
    RUN_TEST(test_automation_source_event_golden);
    RUN_TEST(test_put_off_event_golden);
    RUN_TEST(test_payloads_fit);
    return UNITY_END();
}
