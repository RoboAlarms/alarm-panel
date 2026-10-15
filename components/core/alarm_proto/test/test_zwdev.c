/* MQTT-018..020 (docs/features/10), ZW-036 (docs/features/36): the Z-Wave devices the panel
 * switches, for Home Assistant - their ids (stable, a reused node ID a new one), discovery
 * documents (golden), state payloads, and commands parsed strictly (a garage never opens from
 * here). */
#include <string.h>

#include "unity.h"
#include "alarm_proto/proto_zw.h"

void setUp(void) {}
void tearDown(void) {}

static const proto_ident_t ID = {"alarmpanel", "a1b2c3", "0.5.0", "CrowPanel Advanced 7"};
static char BUF[PROTO_PAYLOAD_MAX];

static const char *doc(const proto_zw_device_t *d)
{
    pj_writer_t w;
    pj_init(&w, BUF, sizeof BUF);
    proto_ha_zw_device(&w, &ID, d);
    TEST_ASSERT_TRUE(pj_finish(&w) > 0u);
    return BUF;
}

static const char *state(const proto_zw_device_t *d)
{
    pj_writer_t w;
    pj_init(&w, BUF, sizeof BUF);
    proto_zw_state(&w, d);
    TEST_ASSERT_TRUE(pj_finish(&w) > 0u);
    return BUF;
}

#define DEVICE_BLOCK                                                                              \
    "\"device\":{\"identifiers\":[\"alarmpanel_a1b2c3\"],\"name\":\"Alarm Panel\","             \
    "\"manufacturer\":\"RoboAlarms Panel (open source)\",\"model\":\"CrowPanel Advanced 7\","    \
    "\"sw_version\":\"0.5.0\"},\"origin\":{\"name\":\"RoboAlarms Panel\",\"sw_version\":\"0.5.0\"}"

/* MQTT-018: ids from the network, device, generation and end point; topics under the panel. */
static void test_keys_and_topics(void)
{
    char key[PROTO_ZW_KEY_LEN];
    TEST_ASSERT_TRUE(proto_zw_key(0xC0FFEE01u, 7u, 2u, 1u, key));
    TEST_ASSERT_EQUAL_STRING("zw_c0ffee01_7_2_1", key);
    char other[PROTO_ZW_KEY_LEN];
    TEST_ASSERT_TRUE(proto_zw_key(0xC0FFEE01u, 7u, 3u, 1u, other));
    TEST_ASSERT_TRUE(strcmp(key, other) != 0); /* node 7 given to another device */
    char topic[PROTO_TOPIC_LEN];
    TEST_ASSERT_TRUE(proto_zw_topic(&ID, key, "set", topic, sizeof topic));
    TEST_ASSERT_EQUAL_STRING("alarmpanel/a1b2c3/zwave/zw_c0ffee01_7_2_1/set", topic);
    TEST_ASSERT_TRUE(proto_ha_topic(&ID, proto_zw_component(PROTO_ZW_GARAGE), key, topic,
                                    sizeof topic));
    TEST_ASSERT_EQUAL_STRING("homeassistant/cover/alarmpanel_a1b2c3/zw_c0ffee01_7_2_1/config",
                             topic);
    TEST_ASSERT_NULL(proto_zw_component(9u));
}

/* MQTT-019: the discovery documents - a switch, a dimmer, a garage that closes but never opens
 * from Home Assistant; available only while the panel is online and the device answers. */
static void test_discovery(void)
{
    proto_zw_device_t d;
    memset(&d, 0, sizeof d);
    snprintf(d.key, sizeof d.key, "zw_c0ffee01_7_2_0");
    snprintf(d.name, sizeof d.name, "Porch light");
    d.kind = PROTO_ZW_SWITCH;
    TEST_ASSERT_EQUAL_STRING(
        "{\"name\":\"Porch light\",\"unique_id\":\"alarmpanel_a1b2c3_zw_c0ffee01_7_2_0\","
        "\"~\":\"alarmpanel/a1b2c3\",\"availability\":[{\"topic\":\"~/availability\"},"
        "{\"topic\":\"~/zwave/zw_c0ffee01_7_2_0/available\"}],\"availability_mode\":\"all\","
        DEVICE_BLOCK ",\"state_topic\":\"~/zwave/zw_c0ffee01_7_2_0/state\","
        "\"command_topic\":\"~/zwave/zw_c0ffee01_7_2_0/set\","
        "\"value_template\":\"{{ value_json.state }}\",\"payload_on\":\"ON\","
        "\"payload_off\":\"OFF\",\"state_on\":\"ON\",\"state_off\":\"OFF\"}",
        doc(&d));
    d.kind = PROTO_ZW_LIGHT;
    TEST_ASSERT_NOT_NULL(strstr(doc(&d), "\"schema\":\"json\",\"brightness\":true,"
                                         "\"brightness_scale\":100,\"supported_color_modes\":"
                                         "[\"brightness\"]}"));
    d.kind = PROTO_ZW_GARAGE;
    const char *g = doc(&d);
    TEST_ASSERT_NOT_NULL(strstr(g, "\"device_class\":\"garage\",\"payload_open\":null,"
                                   "\"payload_stop\":null,\"payload_close\":\"CLOSE\""));
    TEST_ASSERT_NULL(strstr(g, "OPEN"));
    d.kind = 0u;
    pj_writer_t w;
    pj_init(&w, BUF, sizeof BUF);
    proto_ha_zw_device(&w, &ID, &d);
    TEST_ASSERT_EQUAL_UINT(0u, pj_finish(&w)); /* no kind, no document */
}

/* MQTT-019: state as the device last said it; not known is null (Home Assistant's unknown). */
static void test_state(void)
{
    proto_zw_device_t d;
    memset(&d, 0, sizeof d);
    d.kind = PROTO_ZW_SWITCH;
    d.level = -1;
    TEST_ASSERT_EQUAL_STRING("{\"state\":null}", state(&d));
    d.level = 100;
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"ON\"}", state(&d));
    d.kind = PROTO_ZW_LIGHT;
    d.level = 40;
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"ON\",\"brightness\":40,\"color_mode\":\"brightness\"}",
                             state(&d));
    d.level = 0;
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"OFF\",\"brightness\":0,\"color_mode\":\"brightness\"}",
                             state(&d));
    d.kind = PROTO_ZW_GARAGE;
    snprintf(d.garage, sizeof d.garage, "closing");
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"closing\"}", state(&d));
    d.garage[0] = '\0';
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"unknown\"}", state(&d));
}

/* MQTT-020: commands taken exactly as documented, anything else refused - a garage's OPEN too. */
static void test_commands(void)
{
    int level = -5;
    TEST_ASSERT_EQUAL_INT(ALARM_OK, proto_zw_cmd_parse("ON", 2u, PROTO_ZW_SWITCH, &level));
    TEST_ASSERT_EQUAL_INT(100, level);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, proto_zw_cmd_parse("OFF", 3u, PROTO_ZW_SWITCH, &level));
    TEST_ASSERT_EQUAL_INT(0, level);
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_BAD_ARG, proto_zw_cmd_parse("on", 2u, PROTO_ZW_SWITCH, &level));
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_BAD_ARG, proto_zw_cmd_parse("ON ", 3u, PROTO_ZW_SWITCH, &level));
    static const char L1[] = "{\"state\":\"ON\",\"brightness\":40}";
    TEST_ASSERT_EQUAL_INT(ALARM_OK, proto_zw_cmd_parse(L1, strlen(L1), PROTO_ZW_LIGHT, &level));
    TEST_ASSERT_EQUAL_INT(40, level);
    static const char L2[] = "{\"state\":\"ON\"}";
    TEST_ASSERT_EQUAL_INT(ALARM_OK, proto_zw_cmd_parse(L2, strlen(L2), PROTO_ZW_LIGHT, &level));
    TEST_ASSERT_EQUAL_INT(100, level);
    static const char L3[] = "{\"state\":\"OFF\",\"transition\":2}";
    TEST_ASSERT_EQUAL_INT(ALARM_OK, proto_zw_cmd_parse(L3, strlen(L3), PROTO_ZW_LIGHT, &level));
    TEST_ASSERT_EQUAL_INT(0, level);
    static const char *const BAD[] = {"{\"state\":\"ON\",\"brightness\":255}",
                                      "{\"brightness\":40}", "{\"state\":1}", "ON",
                                      "{\"state\":\"ON\"} x", "{\"state\":\"ON\""};
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(ALARM_ERR_BAD_ARG,
                                      proto_zw_cmd_parse(BAD[i], strlen(BAD[i]), PROTO_ZW_LIGHT,
                                                         &level),
                                      BAD[i]);
    }
    TEST_ASSERT_EQUAL_INT(ALARM_OK, proto_zw_cmd_parse("CLOSE", 5u, PROTO_ZW_GARAGE, &level));
    TEST_ASSERT_EQUAL_INT(0, level);
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_BAD_ARG, proto_zw_cmd_parse("OPEN", 4u, PROTO_ZW_GARAGE, &level));
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_BAD_ARG, proto_zw_cmd_parse("STOP", 4u, PROTO_ZW_GARAGE, &level));
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_BAD_ARG, proto_zw_cmd_parse("ON", 2u, PROTO_ZW_GARAGE, &level));
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_BAD_ARG, proto_zw_cmd_parse("ON", 2u, 0u, &level));
    TEST_ASSERT_EQUAL_INT(ALARM_ERR_BAD_ARG, proto_zw_cmd_parse("ON", 0u, PROTO_ZW_SWITCH, &level));
}

/* MQTT-020, ZW-023: random payloads for every kind: never out of bounds, and anything taken is a
 * level 0..100 (a garage's only 0). */
static void test_random_commands(void)
{
    uint64_t seed = 0xC0DEu;
    char p[64];
    for (unsigned iter = 0; iter < 30000u; iter++) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        const size_t len = (size_t)(seed >> 58);
        for (size_t i = 0; i < len; i++) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            static const char A[] = "{}\":,ONFbrightnesstate0123456789 CLOSEPN";
            p[i] = A[(seed >> 33) % (sizeof A - 1u)];
        }
        for (uint8_t kind = 0u; kind <= 4u; kind++) {
            int level = -7;
            if (proto_zw_cmd_parse(p, len, kind, &level) == ALARM_OK) {
                TEST_ASSERT_TRUE(level >= 0 && level <= 100);
                TEST_ASSERT_TRUE(kind != PROTO_ZW_GARAGE || level == 0);
            }
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_keys_and_topics);
    RUN_TEST(test_discovery);
    RUN_TEST(test_state);
    RUN_TEST(test_commands);
    RUN_TEST(test_random_commands);
    return UNITY_END();
}
