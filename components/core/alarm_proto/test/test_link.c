/*
 * test_link.c - the local Home Assistant link (alarm_proto link.c, sha256.c): SHA-256 against
 * the FIPS vectors, hex, framing, the pairing code (golden vectors shared verbatim with the
 * aioroboalarms Python client's tests) and the message writers and parser.
 * Requirements: HAI-002 (framing bounds), HAI-003 (pairing code), HAI-009 (entities as zones:
 * watch, catalog, state), HAI-013 (api version), HAI-016..019 (panel settings and keypads:
 * settings, keypads, settings_set, settings_result and the set queue) (docs/features/23,
 * protocol.md)
 */
#include <stdio.h>
#include <string.h>

#include "alarm_proto/proto_link.h"
#include "unity.h"

static char BUF[512];
static pj_writer_t W;

static const proto_ident_t ID = {"alarmpanel", "a1b2c3", "0.6.0", "CrowPanel Advanced 7"};

void setUp(void)
{
    pj_init(&W, BUF, sizeof(BUF));
}

void tearDown(void) {}

static const char *done(void)
{
    TEST_ASSERT_TRUE_MESSAGE(pj_finish(&W) > 0u, "document overflowed or isn't closed");
    return BUF;
}

/* Bytes 0x00.., 0x20.., 0x40.., 0x60.. as in protocol.md, "Golden vectors". */
static void fill(uint8_t *out, uint8_t first)
{
    for (uint8_t i = 0; i < 32u; i++) {
        out[i] = (uint8_t)(first + i);
    }
}

/* ---- SHA-256 ------------------------------------------------------------------------------ */

static void expect_sha(const char *msg, size_t repeat, const char *hex)
{
    uint8_t data[1000];
    size_t len = strlen(msg);
    if (repeat > 1u) {
        for (size_t i = 0; i < repeat; i++) {
            data[i] = (uint8_t)msg[0];
        }
        len = repeat;
    } else {
        memcpy(data, msg, len);
    }
    uint8_t digest[32];
    proto_sha256(data, len, digest);
    char got[65];
    proto_hex(digest, 32, got);
    TEST_ASSERT_EQUAL_STRING(hex, got);
}

static void test_sha256_vectors(void)
{
    expect_sha("", 1, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    expect_sha("abc", 1, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    /* 56 bytes: the padding needs a second block */
    expect_sha("a", 56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    /* several blocks with a partial tail */
    expect_sha("a", 1000, "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

/* ---- hex ---------------------------------------------------------------------------------- */

static void test_hex(void)
{
    const uint8_t bytes[4] = {0x00, 0x9f, 0xA0, 0xff};
    char hex[9];
    proto_hex(bytes, 4, hex);
    TEST_ASSERT_EQUAL_STRING("009fa0ff", hex);

    uint8_t back[4];
    TEST_ASSERT_TRUE(proto_unhex("009fa0ff", back, 4));
    TEST_ASSERT_EQUAL_MEMORY(bytes, back, 4);
    TEST_ASSERT_TRUE(proto_unhex("009FA0FF", back, 4)); /* either case */

    TEST_ASSERT_FALSE(proto_unhex("009fa0f", back, 4));   /* too short */
    TEST_ASSERT_FALSE(proto_unhex("009fa0ff00", back, 4)); /* too long */
    TEST_ASSERT_FALSE(proto_unhex("009fa0fg", back, 4));   /* not hex */
    TEST_ASSERT_FALSE(proto_unhex("", back, 1));
}

/* ---- framing (HAI-002) -------------------------------------------------------------------- */

static void test_framing(void)
{
    uint8_t hdr[PROTO_LINK_HDR_LEN];
    uint32_t len = 0;

    proto_link_frame_header(1, hdr);
    TEST_ASSERT_EQUAL_UINT8(0, hdr[0]);
    TEST_ASSERT_EQUAL_UINT8(1, hdr[3]);
    TEST_ASSERT_TRUE(proto_link_frame_length(hdr, &len));
    TEST_ASSERT_EQUAL_UINT32(1, len);

    proto_link_frame_header(0x0102a3b4u, hdr); /* big-endian order */
    TEST_ASSERT_EQUAL_UINT8(0x01, hdr[0]);
    TEST_ASSERT_EQUAL_UINT8(0x02, hdr[1]);
    TEST_ASSERT_EQUAL_UINT8(0xa3, hdr[2]);
    TEST_ASSERT_EQUAL_UINT8(0xb4, hdr[3]);

    proto_link_frame_header(PROTO_LINK_FRAME_MAX, hdr);
    TEST_ASSERT_TRUE(proto_link_frame_length(hdr, &len));
    TEST_ASSERT_EQUAL_UINT32(PROTO_LINK_FRAME_MAX, len);

    proto_link_frame_header(0, hdr);
    TEST_ASSERT_FALSE(proto_link_frame_length(hdr, &len)); /* empty frame: close */
    proto_link_frame_header(PROTO_LINK_FRAME_MAX + 1u, hdr);
    TEST_ASSERT_FALSE(proto_link_frame_length(hdr, &len)); /* oversized: close */
}

/* ---- pairing code (HAI-003) --------------------------------------------------------------- */

/* protocol.md "Golden vectors": the same values in the Python client's tests. */
static void test_pair_code_vectors(void)
{
    uint8_t nonce_ha[32];
    uint8_t nonce_panel[32];
    uint8_t fp_ha[32];
    uint8_t fp_panel[32];
    char code[PROTO_LINK_CODE_LEN];

    fill(nonce_ha, 0x00);
    fill(nonce_panel, 0x20);
    fill(fp_ha, 0x40);
    fill(fp_panel, 0x60);
    proto_link_pair_code(nonce_ha, nonce_panel, fp_ha, fp_panel, code);
    TEST_ASSERT_EQUAL_STRING("588460", code); /* vector 1 */

    uint8_t zero[32] = {0};
    proto_link_pair_code(zero, zero, zero, zero, code);
    TEST_ASSERT_EQUAL_STRING("064511", code); /* vector 2: keeps its leading zero */

    proto_link_pair_code(nonce_panel, nonce_ha, fp_ha, fp_panel, code);
    TEST_ASSERT_EQUAL_STRING("195600", code); /* vector 3: swapping the nonces changes it */
}

/* HAI-003: the code depends on every input - a man in the middle with its own certificate
 * on one leg cannot end up with the same code on both. */
static void test_pair_code_binds_certificates(void)
{
    uint8_t nonce_ha[32];
    uint8_t nonce_panel[32];
    uint8_t fp[32];
    uint8_t fp_other[32];
    fill(nonce_ha, 0x00);
    fill(nonce_panel, 0x20);
    fill(fp, 0x40);
    fill(fp_other, 0x40);
    fp_other[31] ^= 0x01u;

    char a[PROTO_LINK_CODE_LEN];
    char b[PROTO_LINK_CODE_LEN];
    proto_link_pair_code(nonce_ha, nonce_panel, fp, fp, a);
    proto_link_pair_code(nonce_ha, nonce_panel, fp_other, fp, b);
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);
    proto_link_pair_code(nonce_ha, nonce_panel, fp, fp_other, b);
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);
}

/* ---- writers ------------------------------------------------------------------------------ */

static void test_hello_golden(void)
{
    proto_link_hello(&W, &ID, "Home", false);
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"hello\",\"api\":1,\"id\":\"a1b2c3\",\"name\":\"Home\","
                             "\"model\":\"CrowPanel Advanced 7\",\"fw\":\"0.6.0\","
                             "\"paired\":false}",
                             done());
}

static void test_pair_nonce_golden(void)
{
    uint8_t nonce[32];
    fill(nonce, 0x20);
    proto_link_pair_nonce(&W, nonce);
    TEST_ASSERT_EQUAL_STRING(
        "{\"t\":\"pair_nonce\",\"nonce\":"
        "\"202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f\"}",
        done());
}

static void test_pair_result_golden(void)
{
    proto_link_pair_result(&W, PROTO_LINK_R_NOT_PAIRING);
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"pair_result\",\"result\":\"not_pairing\"}", done());
}

/* ---- state for the paired Home Assistant --------------------------------------------------- */

static bool valid_json(const char *s)
{
    pj_reader_t r;
    pj_read_init(&r, s, strlen(s));
    pj_skip(&r);
    return pj_finished(&r);
}

static void starts_with(const char *prefix, const char *s)
{
    TEST_ASSERT_EQUAL_STRING_LEN(prefix, s, strlen(prefix));
}

static alarm_config_t CFG;   /* off the stack: ~10 KB each */
static alarm_snapshot_t SNAP;
static char BIG[PROTO_LINK_SNAPSHOT_MAX];

static void test_state_messages(void)
{
    alarm_config_defaults(&CFG);
    alarm_zone_config_defaults(&CFG.zones[0], 1, ALARM_ZT_ENTRY_EXIT_1, 1);
    snprintf(CFG.zones[0].name, ALARM_NAME_LEN, "%s", "Front Door");
    memset(&SNAP, 0, sizeof(SNAP));
    SNAP.partitions = 0x01;
    SNAP.seq = 7;

    pj_writer_t w;
    pj_init(&w, BIG, sizeof(BIG));
    proto_link_snapshot(&w, &CFG, &SNAP, NULL);
    TEST_ASSERT_TRUE(pj_finish(&w) > 0u);
    TEST_ASSERT_TRUE(valid_json(BIG));
    starts_with("{\"t\":\"snapshot\",\"seq\":7,\"partitions\":[{\"partition\":1,", BIG);
    TEST_ASSERT_NOT_NULL(strstr(BIG, "\"zone\":1"));
    TEST_ASSERT_NOT_NULL(strstr(BIG, "Front Door"));
    TEST_ASSERT_NOT_NULL(strstr(BIG, "\"troubles\":{"));

    /* a delta carrying just zone 1, no partitions, no troubles */
    uint8_t zones[ALARM_ZONE_BITMAP_BYTES] = {0x01};
    pj_init(&w, BIG, sizeof(BIG));
    proto_link_delta(&w, &CFG, &SNAP, NULL, 0, zones, false);
    TEST_ASSERT_TRUE(pj_finish(&w) > 0u);
    TEST_ASSERT_TRUE(valid_json(BIG));
    starts_with("{\"t\":\"delta\",\"seq\":7,\"partitions\":[],\"zones\":[{\"zone\":1,", BIG);
    TEST_ASSERT_NULL(strstr(BIG, "\"troubles\""));

    alarm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.seq = 41;
    ev.type = ALARM_EV_ARMED;
    ev.partition = 1;
    ev.arg = ALARM_LEVEL_AWAY;
    pj_init(&w, BIG, sizeof(BIG));
    proto_link_event(&w, &CFG, &ev, true);
    TEST_ASSERT_TRUE(pj_finish(&w) > 0u);
    TEST_ASSERT_TRUE(valid_json(BIG));
    starts_with("{\"t\":\"event\",\"seq\":41,", BIG);
    TEST_ASSERT_NOT_NULL(strstr(BIG, "\"replayed\":true"));

    proto_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    snprintf(cmd.id, sizeof(cmd.id), "8f2c");
    snprintf(cmd.action, sizeof(cmd.action), "arm");
    pj_init(&w, BIG, sizeof(BIG));
    proto_link_result(&w, &cmd, ALARM_ERR_NOT_READY, NULL);
    TEST_ASSERT_TRUE(pj_finish(&w) > 0u);
    starts_with("{\"t\":\"result\",\"id\":\"8f2c\",\"action\":\"arm\",\"result\":\"not_ready\"}",
                BIG);
}

/* PROTO_LINK_SNAPSHOT_MAX holds the biggest panel: every zone and partition configured with
 * the longest names, every flag set, every trouble active. */
static void test_snapshot_fits_full_panel(void)
{
    alarm_config_defaults(&CFG);
    char longname[ALARM_NAME_LEN];
    memset(longname, 'W', sizeof(longname) - 1u);
    longname[sizeof(longname) - 1u] = '\0';
    for (uint8_t p = 1; p <= ALARM_MAX_PARTITIONS; p++) {
        CFG.partitions[p - 1u].enabled = true;
        snprintf(CFG.partitions[p - 1u].name, ALARM_NAME_LEN, "%s", longname);
    }
    for (uint16_t z = 1; z <= ALARM_MAX_ZONES; z++) {
        alarm_zone_config_defaults(&CFG.zones[z - 1u], z, ALARM_ZT_PERIMETER, 4);
        snprintf(CFG.zones[z - 1u].name, ALARM_NAME_LEN, "%s", longname);
    }
    memset(&SNAP, 0, sizeof(SNAP));
    SNAP.partitions = 0x0F;
    SNAP.seq = 4000000000u;
    for (uint16_t z = 0; z < ALARM_MAX_ZONES; z++) {
        SNAP.zone[z].flags = 0xFFFFu; /* every boolean true: the longest form */
        SNAP.zone[z].trouble = 1;
    }

    pj_writer_t w;
    pj_init(&w, BIG, sizeof(BIG));
    proto_link_snapshot(&w, &CFG, &SNAP, NULL);
    const size_t len = pj_finish(&w);
    TEST_ASSERT_TRUE_MESSAGE(len > 0u, "a full panel's snapshot must fit SNAPSHOT_MAX");
    TEST_ASSERT_TRUE(valid_json(BIG));
    TEST_ASSERT_TRUE(len <= PROTO_LINK_FRAME_MAX); /* and inside one frame */
}

/* ---- two-way: Home Assistant entities as zones (HAI-009) ----------------------------------- */

/* protocol.md "Two-way": the panel sends the full list of bound entity_ids every time; an empty
 * list clears the watch. */
static void test_watch_golden(void)
{
    const char *const ids[2] = {"binary_sensor.front_door", "sensor.cellar_temperature"};
    proto_link_watch(&W, ids, 2);
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"watch\",\"ids\":[\"binary_sensor.front_door\","
                             "\"sensor.cellar_temperature\"]}",
                             done());

    pj_init(&W, BUF, sizeof(BUF));
    proto_link_watch(&W, ids, 0);
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"watch\",\"ids\":[]}", done());
}

/* A catalog page as the integration sends it (protocol.md): page and more before the entities. */
static const char CATALOG_JSON[] =
    "{\"t\":\"catalog\",\"page\":0,\"more\":false,\"entities\":["
    "{\"id\":\"binary_sensor.front_door\",\"name\":\"Front Door\",\"area\":\"Hall\","
    "\"domain\":\"binary_sensor\",\"class\":\"door\",\"state\":\"off\"},"
    "{\"id\":\"sensor.cellar_temperature\",\"name\":\"Cellar\",\"area\":\"Cellar\","
    "\"domain\":\"sensor\",\"class\":\"temperature\",\"state\":\"4.5\"}]}";

static bool catalog_init(proto_link_catalog_t *c, const char *json)
{
    return proto_link_catalog_init(c, json, strlen(json));
}

/* HAI-009: every field of every entity of a page comes back, then the walk ends. */
static void test_catalog_round_trip(void)
{
    proto_link_catalog_t c;
    proto_link_entity_t e;

    TEST_ASSERT_TRUE(catalog_init(&c, CATALOG_JSON));
    TEST_ASSERT_EQUAL_INT64(0, c.page);
    TEST_ASSERT_FALSE(c.more);

    TEST_ASSERT_TRUE(proto_link_catalog_next(&c, &e));
    TEST_ASSERT_EQUAL_STRING("binary_sensor.front_door", e.id);
    TEST_ASSERT_EQUAL_STRING("Front Door", e.name);
    TEST_ASSERT_EQUAL_STRING("Hall", e.area);
    TEST_ASSERT_EQUAL_STRING("binary_sensor", e.domain);
    TEST_ASSERT_EQUAL_STRING("door", e.class_);
    TEST_ASSERT_EQUAL_STRING("off", e.state);

    TEST_ASSERT_TRUE(proto_link_catalog_next(&c, &e));
    TEST_ASSERT_EQUAL_STRING("sensor.cellar_temperature", e.id);
    TEST_ASSERT_EQUAL_STRING("Cellar", e.name);
    TEST_ASSERT_EQUAL_STRING("Cellar", e.area);
    TEST_ASSERT_EQUAL_STRING("sensor", e.domain);
    TEST_ASSERT_EQUAL_STRING("temperature", e.class_);
    TEST_ASSERT_EQUAL_STRING("4.5", e.state);

    TEST_ASSERT_FALSE(proto_link_catalog_next(&c, &e)); /* the page is done */
    TEST_ASSERT_FALSE(proto_link_catalog_next(&c, &e));
}

/* HAI-009: a later page says so; an empty page carries nothing; anything that isn't a catalog
 * is refused before the walk starts. */
static void test_catalog_pages_and_refusals(void)
{
    proto_link_catalog_t c;
    proto_link_entity_t e;

    TEST_ASSERT_TRUE(catalog_init(&c, "{\"t\":\"catalog\",\"page\":3,\"more\":true,"
                                      "\"entities\":[]}"));
    TEST_ASSERT_EQUAL_INT64(3, c.page);
    TEST_ASSERT_TRUE(c.more);
    TEST_ASSERT_FALSE(proto_link_catalog_next(&c, &e));

    TEST_ASSERT_FALSE(catalog_init(&c, "{\"t\":\"state\",\"id\":\"binary_sensor.front_door\"}"));
    TEST_ASSERT_FALSE(proto_link_catalog_next(&c, &e));
    TEST_ASSERT_FALSE(catalog_init(&c, "[{\"id\":\"binary_sensor.front_door\"}]"));
    TEST_ASSERT_FALSE(catalog_init(&c, "{\"page\":0,\"entities\":[]}")); /* no "t" */

    /* A damaged frame never reaches the walk: proto_link_parse refuses it and the connection
     * closes (proto_link.h). What the walk itself sees of it is an empty page. */
    proto_link_in_t in;
    const char *cut = "{\"t\":\"catalog\",\"page\":0,\"entities\":[{\"id\":\"lock.front\"";
    TEST_ASSERT_FALSE(proto_link_parse(cut, strlen(cut), &in));
    (void)catalog_init(&c, cut);
    TEST_ASSERT_FALSE(proto_link_catalog_next(&c, &e)); /* the damage ends the walk */
}

/* HAI-009: keys this firmware doesn't know ride along (forward compatibility); an entity_id
 * longer than a binding can hold comes back empty, so the picker never offers it. */
static void test_catalog_unknown_keys_and_overlong_id(void)
{
    proto_link_catalog_t c;
    proto_link_entity_t e;

    TEST_ASSERT_TRUE(catalog_init(&c, "{\"t\":\"catalog\",\"page\":0,\"more\":false,"
                                      "\"future\":{\"x\":[1,null]},\"entities\":["
                                      "{\"id\":\"lock.front\",\"later\":{\"a\":[1,2]},"
                                      "\"name\":\"Front lock\",\"extra\":true}]}"));
    TEST_ASSERT_TRUE(proto_link_catalog_next(&c, &e));
    TEST_ASSERT_EQUAL_STRING("lock.front", e.id);
    TEST_ASSERT_EQUAL_STRING("Front lock", e.name);
    TEST_ASSERT_EQUAL_STRING("", e.area);
    TEST_ASSERT_FALSE(proto_link_catalog_next(&c, &e));

    char longid[ZSRC_REF_LEN_LINK + 24];
    memset(longid, 'z', sizeof(longid) - 1u);
    longid[sizeof(longid) - 1u] = '\0';
    char json[sizeof(longid) + 128];
    snprintf(json, sizeof(json),
             "{\"t\":\"catalog\",\"page\":0,\"more\":false,\"entities\":"
             "[{\"id\":\"%s\",\"name\":\"Too long\"}]}",
             longid);
    TEST_ASSERT_TRUE(catalog_init(&c, json));
    TEST_ASSERT_TRUE(proto_link_catalog_next(&c, &e));
    TEST_ASSERT_EQUAL_STRING("", e.id); /* not a binding: the caller skips it */
    TEST_ASSERT_EQUAL_STRING("Too long", e.name);
}

/* HAI-009: a state push; avail false is "unknown or unavailable over there" (the zone runs its
 * grace into CHECK); a state without an id names nothing and is refused. */
static void test_parse_state_message(void)
{
    char id[ZSRC_REF_LEN_LINK];
    char state[24];
    bool avail = false;

    const char *ok = "{\"t\":\"state\",\"id\":\"binary_sensor.front_door\",\"state\":\"on\","
                     "\"avail\":true}";
    TEST_ASSERT_TRUE(proto_link_parse_state(ok, strlen(ok), id, state, &avail, NULL));
    TEST_ASSERT_EQUAL_STRING("binary_sensor.front_door", id);
    TEST_ASSERT_EQUAL_STRING("on", state);
    TEST_ASSERT_TRUE(avail);

    /* no avail at all: available (protocol.md only sends it when false) */
    const char *bare = "{\"t\":\"state\",\"id\":\"lock.front\",\"state\":\"locked\"}";
    TEST_ASSERT_TRUE(proto_link_parse_state(bare, strlen(bare), id, state, &avail, NULL));
    TEST_ASSERT_TRUE(avail);

    const char *gone = "{\"t\":\"state\",\"id\":\"binary_sensor.shed\",\"state\":\"unavailable\","
                       "\"avail\":false}";
    TEST_ASSERT_TRUE(proto_link_parse_state(gone, strlen(gone), id, state, &avail, NULL));
    TEST_ASSERT_EQUAL_STRING("binary_sensor.shed", id);
    TEST_ASSERT_FALSE(avail);

    /* unknown keys are skipped */
    const char *extra = "{\"t\":\"state\",\"later\":[1,{\"a\":null}],\"id\":\"sensor.cellar\","
                        "\"state\":\"4.5\",\"attrs\":{\"unit\":\"C\"}}";
    TEST_ASSERT_TRUE(proto_link_parse_state(extra, strlen(extra), id, state, &avail, NULL));
    TEST_ASSERT_EQUAL_STRING("sensor.cellar", id);
    TEST_ASSERT_EQUAL_STRING("4.5", state);

    /* a state longer than the field maps like unknown, it is never truncated into a lie */
    const char *big = "{\"t\":\"state\",\"id\":\"sensor.cellar\","
                      "\"state\":\"a-state-far-longer-than-the-field\"}";
    TEST_ASSERT_TRUE(proto_link_parse_state(big, strlen(big), id, state, &avail, NULL));
    TEST_ASSERT_EQUAL_STRING("sensor.cellar", id);
    TEST_ASSERT_EQUAL_STRING("", state);

    /* nothing to map onto a zone, or not a message at all */
    const char *no_id = "{\"t\":\"state\",\"state\":\"on\"}";
    TEST_ASSERT_FALSE(proto_link_parse_state(no_id, strlen(no_id), id, state, &avail, NULL));
    TEST_ASSERT_FALSE(proto_link_parse_state("[]", 2, id, state, &avail, NULL));
}

/* ---- a controllable entity's values (protocol.md "the Devices screen"; HCTL-002) ---------- */

static void assert_values_none(const proto_link_values_t *v)
{
    TEST_ASSERT_EQUAL_INT8(PROTO_LINK_LEVEL_UNKNOWN, v->level);
    TEST_ASSERT_EQUAL_HEX8(0, v->modes);
    TEST_ASSERT_EQUAL_INT32(PROTO_LINK_TEMP_UNKNOWN, v->setpoint);
    TEST_ASSERT_EQUAL_INT32(PROTO_LINK_TEMP_UNKNOWN, v->temp);
    TEST_ASSERT_EQUAL_HEX8(0, v->caps);        /* "supports" not known */
    TEST_ASSERT_EQUAL_UINT8(0, v->hvac_action); /* not known */
    TEST_ASSERT_EQUAL_UINT32(0, v->since);      /* not known */
}

static bool parse_values(const char *json, proto_link_values_t *v)
{
    char id[ZSRC_REF_LEN_LINK];
    char state[24];
    bool avail = false;
    return proto_link_parse_state(json, strlen(json), id, state, &avail, v);
}

/* HCTL-002: a state carries each value; absent, null, a fraction, a string or out of range is
 * not known; a client that sends none of them parses as before. */
static void test_state_values(void)
{
    proto_link_values_t v;
    char id[ZSRC_REF_LEN_LINK];
    char state[24];
    bool avail = false;

    const char *all = "{\"t\":\"state\",\"id\":\"climate.hall\",\"state\":\"heat\","
                      "\"setpoint\":2150,\"temp\":-325,\"modes\":[\"off\",\"heat\",\"cool\","
                      "\"auto\",\"heat_cool\"]}";
    proto_link_in_t in;
    TEST_ASSERT_TRUE(proto_link_parse(all, strlen(all), &in)); /* the frame gets that far */
    TEST_ASSERT_EQUAL(PROTO_LINK_IN_STATE, in.type);
    TEST_ASSERT_TRUE(proto_link_parse_state(all, strlen(all), id, state, &avail, &v));
    TEST_ASSERT_EQUAL_STRING("climate.hall", id);
    TEST_ASSERT_EQUAL_STRING("heat", state);
    TEST_ASSERT_EQUAL_INT32(2150, v.setpoint);
    TEST_ASSERT_EQUAL_INT32(-325, v.temp);
    TEST_ASSERT_EQUAL_HEX8(PROTO_LINK_MODE_OFF | PROTO_LINK_MODE_HEAT | PROTO_LINK_MODE_COOL |
                               PROTO_LINK_MODE_AUTO | PROTO_LINK_MODE_HEAT_COOL,
                           v.modes);
    TEST_ASSERT_EQUAL_INT8(PROTO_LINK_LEVEL_UNKNOWN, v.level);

    TEST_ASSERT_TRUE(parse_values("{\"t\":\"state\",\"id\":\"light.porch\",\"state\":\"on\","
                                  "\"level\":0}", &v));
    TEST_ASSERT_EQUAL_INT8(0, v.level);
    TEST_ASSERT_TRUE(parse_values("{\"t\":\"state\",\"id\":\"light.porch\",\"state\":\"on\","
                                  "\"level\":100}", &v));
    TEST_ASSERT_EQUAL_INT8(100, v.level);

    /* none of them: today's client */
    TEST_ASSERT_TRUE(parse_values("{\"t\":\"state\",\"id\":\"light.porch\",\"state\":\"off\"}", &v));
    assert_values_none(&v);

    /* null, out of range, a fraction, a string: not known, and the message still reads */
    TEST_ASSERT_TRUE(parse_values("{\"t\":\"state\",\"id\":\"light.porch\",\"state\":\"on\","
                                  "\"level\":null,\"setpoint\":null,\"temp\":null,\"modes\":null}",
                                  &v));
    assert_values_none(&v);
    TEST_ASSERT_TRUE(parse_values("{\"t\":\"state\",\"id\":\"light.porch\",\"state\":\"on\","
                                  "\"level\":101,\"setpoint\":100001,\"temp\":-100001,"
                                  "\"modes\":\"heat\"}",
                                  &v));
    assert_values_none(&v);
    TEST_ASSERT_TRUE(parse_values("{\"t\":\"state\",\"id\":\"light.porch\",\"state\":\"on\","
                                  "\"level\":-1,\"setpoint\":21.5,\"temp\":\"20\","
                                  "\"modes\":{\"a\":1}}",
                                  &v));
    assert_values_none(&v);
    TEST_ASSERT_TRUE(parse_values("{\"t\":\"state\",\"id\":\"light.porch\",\"state\":\"on\","
                                  "\"level\":99999999999999999999999}",
                                  &v));
    assert_values_none(&v);

    /* the edges of the range still read */
    TEST_ASSERT_TRUE(parse_values("{\"t\":\"state\",\"id\":\"climate.hall\",\"state\":\"cool\","
                                  "\"setpoint\":100000,\"temp\":-100000}",
                                  &v));
    TEST_ASSERT_EQUAL_INT32(PROTO_LINK_TEMP_LIMIT, v.setpoint);
    TEST_ASSERT_EQUAL_INT32(-PROTO_LINK_TEMP_LIMIT, v.temp);

    /* a damaged frame is still refused as before */
    const char *cut = "{\"t\":\"state\",\"id\":\"climate.hall\",\"modes\":[\"heat\"";
    TEST_ASSERT_FALSE(proto_link_parse_state(cut, strlen(cut), id, state, &avail, &v));
}

/* HCTL-002: "modes" keeps the words the panel can show and ignores the rest (dry, fan_only, a
 * later word, a long word, another type); an empty array or one with only those is not known;
 * an array longer than PROTO_LINK_MODES_MAX is not known at all. */
static void test_state_modes(void)
{
    proto_link_values_t v;
    TEST_ASSERT_TRUE(parse_values("{\"t\":\"state\",\"id\":\"climate.hall\",\"state\":\"heat\","
                                  "\"modes\":[\"dry\",\"heat\",\"fan_only\",\"eco_boost\","
                                  "\"a-mode-word-much-longer-than-any\",7,null,[\"cool\"],"
                                  "{\"m\":\"cool\"},\"OFF\",\"heat\"]}",
                                  &v));
    TEST_ASSERT_EQUAL_HEX8(PROTO_LINK_MODE_HEAT, v.modes);

    TEST_ASSERT_TRUE(parse_values("{\"t\":\"state\",\"id\":\"climate.hall\",\"state\":\"heat\","
                                  "\"modes\":[]}", &v));
    TEST_ASSERT_EQUAL_HEX8(0, v.modes);
    TEST_ASSERT_TRUE(parse_values("{\"t\":\"state\",\"id\":\"climate.hall\",\"state\":\"dry\","
                                  "\"modes\":[\"dry\",\"fan_only\"]}", &v));
    TEST_ASSERT_EQUAL_HEX8(0, v.modes);

    /* exactly the most: read; one more: not known */
    char json[512];
    char list[400] = "";
    for (int i = 0; i < PROTO_LINK_MODES_MAX; i++) {
        strcat(list, i == 0 ? "\"cool\"" : ",\"dry\"");
    }
    snprintf(json, sizeof(json), "{\"t\":\"state\",\"id\":\"climate.hall\",\"modes\":[%s]}", list);
    TEST_ASSERT_TRUE(parse_values(json, &v));
    TEST_ASSERT_EQUAL_HEX8(PROTO_LINK_MODE_COOL, v.modes);
    snprintf(json, sizeof(json), "{\"t\":\"state\",\"id\":\"climate.hall\",\"modes\":[%s,\"heat\"],"
                                 "\"setpoint\":1900}", list);
    TEST_ASSERT_TRUE(parse_values(json, &v));
    TEST_ASSERT_EQUAL_HEX8(0, v.modes);
    TEST_ASSERT_EQUAL_INT32(1900, v.setpoint); /* the rest of the message still reads */
}

/* HCTL-002: a control entity of a catalog page carries the same values; one without them, and
 * a zone-only entity, has none known. */
static void test_catalog_values(void)
{
    proto_link_catalog_t c;
    proto_link_entity_t e;
    TEST_ASSERT_TRUE(catalog_init(
        &c, "{\"t\":\"catalog\",\"page\":0,\"more\":false,\"entities\":["
            "{\"id\":\"light.porch\",\"name\":\"Porch\",\"domain\":\"light\",\"state\":\"on\","
            "\"zones\":false,\"control\":true,\"level\":40},"
            "{\"id\":\"climate.hall\",\"domain\":\"climate\",\"state\":\"heat_cool\","
            "\"control\":true,\"temp\":2075,\"modes\":[\"off\",\"heat_cool\",\"fan_only\"]},"
            "{\"id\":\"cover.garage\",\"domain\":\"cover\",\"state\":\"open\",\"control\":true,"
            "\"level\":null},"
            "{\"id\":\"binary_sensor.front_door\",\"domain\":\"binary_sensor\",\"state\":\"off\"}]}"));

    TEST_ASSERT_TRUE(proto_link_catalog_next(&c, &e));
    TEST_ASSERT_EQUAL_STRING("light.porch", e.id);
    TEST_ASSERT_TRUE(e.control);
    TEST_ASSERT_FALSE(e.zones);
    TEST_ASSERT_EQUAL_INT8(40, e.values.level);
    TEST_ASSERT_EQUAL_INT32(PROTO_LINK_TEMP_UNKNOWN, e.values.setpoint);

    TEST_ASSERT_TRUE(proto_link_catalog_next(&c, &e));
    TEST_ASSERT_EQUAL_STRING("climate.hall", e.id);
    TEST_ASSERT_EQUAL_INT32(2075, e.values.temp);
    TEST_ASSERT_EQUAL_INT32(PROTO_LINK_TEMP_UNKNOWN, e.values.setpoint); /* heat_cool: a range */
    TEST_ASSERT_EQUAL_HEX8(PROTO_LINK_MODE_OFF | PROTO_LINK_MODE_HEAT_COOL, e.values.modes);
    TEST_ASSERT_EQUAL_INT8(PROTO_LINK_LEVEL_UNKNOWN, e.values.level);

    TEST_ASSERT_TRUE(proto_link_catalog_next(&c, &e));
    TEST_ASSERT_EQUAL_STRING("cover.garage", e.id);
    assert_values_none(&e.values);

    TEST_ASSERT_TRUE(proto_link_catalog_next(&c, &e));
    TEST_ASSERT_EQUAL_STRING("binary_sensor.front_door", e.id);
    assert_values_none(&e.values);
    TEST_ASSERT_FALSE(proto_link_catalog_next(&c, &e));
}

static uint8_t supports_of(const char *list)
{
    char json[1024];
    proto_link_values_t v;
    snprintf(json, sizeof(json), "{\"t\":\"state\",\"id\":\"cover.garage\",\"state\":\"open\"%s%s}",
             list != NULL ? ",\"supports\":" : "", list != NULL ? list : "");
    TEST_ASSERT_TRUE(parse_values(json, &v));
    return v.caps;
}

/* HCTL-002: "supports" says what the entity can do; present (even []) it is known and a bit not
 * set can't be done; absent, another type or more than PROTO_LINK_SUPPORTS_MAX elements is not
 * known (0); unknown words, non-strings and overlong words are ignored, the list still known. */
static void test_state_supports(void)
{
    TEST_ASSERT_EQUAL_HEX8(PROTO_LINK_CAP_KNOWN | PROTO_LINK_CAP_STOP | PROTO_LINK_CAP_POSITION,
                           supports_of("[\"stop\",\"set_position\"]"));
    TEST_ASSERT_EQUAL_HEX8(PROTO_LINK_CAP_KNOWN | PROTO_LINK_CAP_PERCENTAGE,
                           supports_of("[\"set_percentage\"]"));
    TEST_ASSERT_EQUAL_HEX8(PROTO_LINK_CAP_KNOWN | PROTO_LINK_CAP_BRIGHTNESS,
                           supports_of("[\"brightness\"]"));
    TEST_ASSERT_EQUAL_HEX8(PROTO_LINK_CAP_KNOWN, supports_of("[]"));

    /* not known: absent (an older client), another type, too many elements */
    TEST_ASSERT_EQUAL_HEX8(0, supports_of(NULL));
    TEST_ASSERT_EQUAL_HEX8(0, supports_of("\"stop\""));
    TEST_ASSERT_EQUAL_HEX8(0, supports_of("null"));
    TEST_ASSERT_EQUAL_HEX8(0, supports_of("{\"stop\":true}"));
    char list[400] = "[";
    for (int i = 0; i <= PROTO_LINK_SUPPORTS_MAX; i++) { /* one more than the most */
        strcat(list, i == 0 ? "\"stop\"" : ",\"tilt\"");
    }
    strcat(list, "]");
    TEST_ASSERT_EQUAL_HEX8(0, supports_of(list));
    /* exactly the most still reads */
    char most[400] = "[";
    for (int i = 0; i < PROTO_LINK_SUPPORTS_MAX; i++) {
        strcat(most, i == 0 ? "\"stop\"" : ",\"tilt\"");
    }
    strcat(most, "]");
    TEST_ASSERT_EQUAL_HEX8(PROTO_LINK_CAP_KNOWN | PROTO_LINK_CAP_STOP, supports_of(most));

    /* an unknown word, a number, an overlong word, a case change: ignored, the list known */
    TEST_ASSERT_EQUAL_HEX8(PROTO_LINK_CAP_KNOWN, supports_of("[\"tilt\"]"));
    TEST_ASSERT_EQUAL_HEX8(PROTO_LINK_CAP_KNOWN, supports_of("[3]"));
    TEST_ASSERT_EQUAL_HEX8(PROTO_LINK_CAP_KNOWN,
                           supports_of("[\"set_percentage_and_far_beyond\",null,[\"stop\"],"
                                       "{\"w\":\"stop\"},\"STOP\"]"));
    TEST_ASSERT_EQUAL_HEX8(PROTO_LINK_CAP_KNOWN | PROTO_LINK_CAP_POSITION,
                           supports_of("[\"tilt\",7,\"set_position\"]"));

    /* the rest of the message still reads after a refused list */
    proto_link_values_t v;
    TEST_ASSERT_TRUE(parse_values("{\"t\":\"state\",\"id\":\"cover.garage\",\"supports\":\"stop\","
                                  "\"level\":30}",
                                  &v));
    TEST_ASSERT_EQUAL_INT8(30, v.level);
}

/* HCTL-002: a catalog entity carries "supports"; the next entity, without it, reads not known
 * (each entity starts afresh). */
static void test_catalog_supports(void)
{
    proto_link_catalog_t c;
    proto_link_entity_t e;
    TEST_ASSERT_TRUE(catalog_init(
        &c, "{\"t\":\"catalog\",\"page\":0,\"more\":false,\"entities\":["
            "{\"id\":\"cover.garage\",\"domain\":\"cover\",\"state\":\"closed\",\"zones\":false,"
            "\"control\":true,\"level\":0,\"supports\":[\"set_position\"]},"
            "{\"id\":\"cover.awning\",\"domain\":\"cover\",\"state\":\"open\",\"control\":true}]}"));
    TEST_ASSERT_TRUE(proto_link_catalog_next(&c, &e));
    TEST_ASSERT_EQUAL_STRING("cover.garage", e.id);
    TEST_ASSERT_EQUAL_HEX8(PROTO_LINK_CAP_KNOWN | PROTO_LINK_CAP_POSITION, e.values.caps);
    TEST_ASSERT_EQUAL_INT8(0, e.values.level);
    TEST_ASSERT_TRUE(proto_link_catalog_next(&c, &e));
    TEST_ASSERT_EQUAL_STRING("cover.awning", e.id);
    assert_values_none(&e.values);
    TEST_ASSERT_FALSE(proto_link_catalog_next(&c, &e));
}

static uint32_t since_of(const char *value)
{
    char json[256];
    proto_link_values_t v;
    snprintf(json, sizeof(json),
             "{\"t\":\"state\",\"id\":\"cover.awning\",\"state\":\"\",\"avail\":false,"
             "\"since\":%s,\"level\":5}",
             value);
    TEST_ASSERT_TRUE(parse_values(json, &v));
    TEST_ASSERT_EQUAL_INT8(5, v.level); /* the rest of the message still reads */
    return v.since;
}

/* HCTL-006: "since" is Unix seconds; 0, negative, a fraction, past 32 bits or another type is
 * not known; a catalog entity carries it too. */
static void test_state_since(void)
{
    TEST_ASSERT_EQUAL_UINT32(1791320712u, since_of("1791320712"));
    TEST_ASSERT_EQUAL_UINT32(1u, since_of("1"));
    TEST_ASSERT_EQUAL_UINT32(4294967295u, since_of("4294967295"));
    TEST_ASSERT_EQUAL_UINT32(0u, since_of("0"));
    TEST_ASSERT_EQUAL_UINT32(0u, since_of("-1"));
    TEST_ASSERT_EQUAL_UINT32(0u, since_of("1.5"));
    TEST_ASSERT_EQUAL_UINT32(0u, since_of("4294967296"));
    TEST_ASSERT_EQUAL_UINT32(0u, since_of("\"x\""));
    TEST_ASSERT_EQUAL_UINT32(0u, since_of("null"));
    TEST_ASSERT_EQUAL_UINT32(0u, since_of("[1791320712]"));

    proto_link_catalog_t c;
    proto_link_entity_t e;
    TEST_ASSERT_TRUE(catalog_init(
        &c, "{\"t\":\"catalog\",\"page\":0,\"more\":false,\"entities\":["
            "{\"id\":\"scene.good_night\",\"domain\":\"scene\","
            "\"state\":\"2026-10-06T21:05:12.345678+00:00\",\"control\":true,"
            "\"since\":1791320712}]}"));
    TEST_ASSERT_TRUE(proto_link_catalog_next(&c, &e));
    TEST_ASSERT_EQUAL_STRING("scene.good_night", e.id);
    TEST_ASSERT_EQUAL_UINT32(1791320712u, e.values.since);
}

static uint8_t hvac_of(const char *value)
{
    char json[256];
    proto_link_values_t v;
    snprintf(json, sizeof(json),
             "{\"t\":\"state\",\"id\":\"climate.hall\",\"state\":\"heat\",\"hvac_action\":%s,"
             "\"setpoint\":2150}",
             value);
    TEST_ASSERT_TRUE(parse_values(json, &v));
    TEST_ASSERT_EQUAL_INT32(2150, v.setpoint); /* the rest of the message still reads */
    return v.hvac_action;
}

/* HCTL-002: "hvac_action" maps each of Home Assistant's eight words to 1..8; another word or
 * type is not known (0). */
static void test_state_hvac_action(void)
{
    TEST_ASSERT_EQUAL_UINT8(PROTO_LINK_HVAC_OFF, hvac_of("\"off\""));
    TEST_ASSERT_EQUAL_UINT8(PROTO_LINK_HVAC_HEATING, hvac_of("\"heating\""));
    TEST_ASSERT_EQUAL_UINT8(PROTO_LINK_HVAC_COOLING, hvac_of("\"cooling\""));
    TEST_ASSERT_EQUAL_UINT8(PROTO_LINK_HVAC_DRYING, hvac_of("\"drying\""));
    TEST_ASSERT_EQUAL_UINT8(PROTO_LINK_HVAC_IDLE, hvac_of("\"idle\""));
    TEST_ASSERT_EQUAL_UINT8(PROTO_LINK_HVAC_FAN, hvac_of("\"fan\""));
    TEST_ASSERT_EQUAL_UINT8(PROTO_LINK_HVAC_PREHEATING, hvac_of("\"preheating\""));
    TEST_ASSERT_EQUAL_UINT8(PROTO_LINK_HVAC_DEFROSTING, hvac_of("\"defrosting\""));
    TEST_ASSERT_EQUAL_UINT8(0, hvac_of("\"dry\""));
    TEST_ASSERT_EQUAL_UINT8(0, hvac_of("\"Heating\""));
    TEST_ASSERT_EQUAL_UINT8(0, hvac_of("\"defrosting-and-far-beyond\""));
    TEST_ASSERT_EQUAL_UINT8(0, hvac_of("\"\""));
    TEST_ASSERT_EQUAL_UINT8(0, hvac_of("3"));
    TEST_ASSERT_EQUAL_UINT8(0, hvac_of("null"));
    TEST_ASSERT_EQUAL_UINT8(0, hvac_of("[\"idle\"]"));
}

/* HCTL-002: the three new keys beside one this panel doesn't know (an 18-byte key whose value
 * is an object): the unknown one is skipped, the new ones read, a value after them still reads.
 * The zone path (no values asked for) and the frame check skip them all, as an older panel's
 * parser skips keys it doesn't know. */
static void test_new_keys_beside_an_unknown_one(void)
{
    const char *st = "{\"t\":\"state\",\"id\":\"cover.awning\",\"state\":\"open\","
                     "\"supported_features\":{\"a\":[1,2]},\"supports\":[\"stop\"],"
                     "\"hvac_action\":\"idle\",\"since\":1791313920,\"level\":42}";
    proto_link_values_t v;
    TEST_ASSERT_TRUE(parse_values(st, &v));
    TEST_ASSERT_EQUAL_HEX8(PROTO_LINK_CAP_KNOWN | PROTO_LINK_CAP_STOP, v.caps);
    TEST_ASSERT_EQUAL_UINT8(PROTO_LINK_HVAC_IDLE, v.hvac_action);
    TEST_ASSERT_EQUAL_UINT32(1791313920u, v.since);
    TEST_ASSERT_EQUAL_INT8(42, v.level);

    proto_link_in_t in;
    TEST_ASSERT_TRUE(proto_link_parse(st, strlen(st), &in));
    TEST_ASSERT_EQUAL(PROTO_LINK_IN_STATE, in.type);
    char id[ZSRC_REF_LEN_LINK];
    char state[24];
    bool avail = false;
    TEST_ASSERT_TRUE(proto_link_parse_state(st, strlen(st), id, state, &avail, NULL));
    TEST_ASSERT_EQUAL_STRING("cover.awning", id);
    TEST_ASSERT_EQUAL_STRING("open", state);
    TEST_ASSERT_TRUE(avail);

    proto_link_catalog_t c;
    proto_link_entity_t e;
    TEST_ASSERT_TRUE(catalog_init(
        &c, "{\"t\":\"catalog\",\"page\":0,\"more\":false,\"entities\":["
            "{\"id\":\"climate.hall\",\"supported_features\":{\"a\":[1,2]},\"domain\":\"climate\","
            "\"supports\":[],\"hvac_action\":\"heating\",\"since\":1791313920,\"control\":true,"
            "\"level\":7,\"name\":\"Hall\"}]}"));
    TEST_ASSERT_TRUE(proto_link_catalog_next(&c, &e));
    TEST_ASSERT_EQUAL_STRING("climate.hall", e.id);
    TEST_ASSERT_EQUAL_STRING("Hall", e.name);
    TEST_ASSERT_TRUE(e.control);
    TEST_ASSERT_EQUAL_HEX8(PROTO_LINK_CAP_KNOWN, e.values.caps);
    TEST_ASSERT_EQUAL_UINT8(PROTO_LINK_HVAC_HEATING, e.values.hvac_action);
    TEST_ASSERT_EQUAL_UINT32(1791313920u, e.values.since);
    TEST_ASSERT_EQUAL_INT8(7, e.values.level);
    TEST_ASSERT_FALSE(proto_link_catalog_next(&c, &e));
}

/* ---- parser ------------------------------------------------------------------------------- */

static bool parse(const char *json, proto_link_in_t *in)
{
    return proto_link_parse(json, strlen(json), in);
}

static void test_parse_hello(void)
{
    proto_link_in_t in;
    TEST_ASSERT_TRUE(parse("{\"t\":\"hello\",\"api\":1,\"client\":\"aioroboalarms 0.1.0\"}", &in));
    TEST_ASSERT_EQUAL_INT(PROTO_LINK_IN_HELLO, in.type);
    TEST_ASSERT_EQUAL_INT64(1, in.api);
    TEST_ASSERT_EQUAL_STRING("aioroboalarms 0.1.0", in.client);

    /* HAI-013: hello without an api (or api 0) is not usable */
    TEST_ASSERT_FALSE(parse("{\"t\":\"hello\",\"client\":\"x\"}", &in));
    TEST_ASSERT_FALSE(parse("{\"t\":\"hello\",\"api\":0}", &in));
    /* a newer client is for the caller to judge: the parse hands the number over */
    TEST_ASSERT_TRUE(parse("{\"t\":\"hello\",\"api\":2}", &in));
    TEST_ASSERT_EQUAL_INT64(2, in.api);
    /* an overlong client name is dropped, not truncated into a lie */
    TEST_ASSERT_TRUE(parse("{\"t\":\"hello\",\"api\":1,\"client\":"
                           "\"0123456789012345678901234567890123456789012345678\"}",
                           &in));
    TEST_ASSERT_EQUAL_STRING("", in.client);
}

static void test_parse_pairing(void)
{
    proto_link_in_t in;
    TEST_ASSERT_TRUE(parse("{\"t\":\"pair_start\",\"commit\":"
                           "\"630dcd2966c4336691125448bbb25b4ff412a49c732db2c8abc1b8581bd710dd\"}",
                           &in));
    TEST_ASSERT_EQUAL_INT(PROTO_LINK_IN_PAIR_START, in.type);
    TEST_ASSERT_EQUAL_UINT8(0x63, in.commit[0]);
    TEST_ASSERT_EQUAL_UINT8(0xdd, in.commit[31]);

    TEST_ASSERT_TRUE(parse("{\"t\":\"pair_reveal\",\"nonce\":"
                           "\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\"}",
                           &in));
    TEST_ASSERT_EQUAL_INT(PROTO_LINK_IN_PAIR_REVEAL, in.type);
    TEST_ASSERT_EQUAL_UINT8(0x00, in.nonce[0]);
    TEST_ASSERT_EQUAL_UINT8(0x1f, in.nonce[31]);

    /* required field missing, too short, or not hex: invalid, close the connection */
    TEST_ASSERT_FALSE(parse("{\"t\":\"pair_start\"}", &in));
    TEST_ASSERT_FALSE(parse("{\"t\":\"pair_start\",\"commit\":\"630d\"}", &in));
    TEST_ASSERT_FALSE(parse("{\"t\":\"pair_reveal\",\"nonce\":"
                            "\"zz0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\"}",
                            &in));
}

static void test_parse_shapes(void)
{
    proto_link_in_t in;
    TEST_ASSERT_TRUE(parse("{\"t\":\"ping\"}", &in));
    TEST_ASSERT_EQUAL_INT(PROTO_LINK_IN_PING, in.type);

    /* a command is recognized; its body goes to proto_cmd_parse afterwards */
    TEST_ASSERT_TRUE(parse("{\"t\":\"command\",\"action\":\"arm\",\"code\":\"246810\"}", &in));
    TEST_ASSERT_EQUAL_INT(PROTO_LINK_IN_COMMAND, in.type);

    /* HAI-009: the two-way frames are recognized; their bodies go to proto_link_catalog_init
     * and proto_link_parse_state afterwards */
    TEST_ASSERT_TRUE(parse("{\"t\":\"catalog\",\"page\":0,\"more\":false,\"entities\":[]}", &in));
    TEST_ASSERT_EQUAL_INT(PROTO_LINK_IN_CATALOG, in.type);
    TEST_ASSERT_TRUE(parse(CATALOG_JSON, &in));
    TEST_ASSERT_EQUAL_INT(PROTO_LINK_IN_CATALOG, in.type);
    TEST_ASSERT_TRUE(parse("{\"t\":\"state\",\"id\":\"binary_sensor.front_door\","
                           "\"state\":\"on\",\"avail\":true}",
                           &in));
    TEST_ASSERT_EQUAL_INT(PROTO_LINK_IN_STATE, in.type);

    /* unknown types and keys ride along (forward compatibility) */
    TEST_ASSERT_TRUE(parse("{\"t\":\"sparkle\",\"x\":[1,{\"y\":null}]}", &in));
    TEST_ASSERT_EQUAL_INT(PROTO_LINK_IN_UNKNOWN, in.type);
    TEST_ASSERT_TRUE(parse("{\"t\":\"a-type-name-longer-than-the-buffer\"}", &in));
    TEST_ASSERT_EQUAL_INT(PROTO_LINK_IN_UNKNOWN, in.type);
    TEST_ASSERT_TRUE(parse("{\"extra\":true,\"t\":\"ping\"}", &in));
    TEST_ASSERT_EQUAL_INT(PROTO_LINK_IN_PING, in.type);

    /* not a message: bad JSON, no object, no "t", trailing garbage */
    TEST_ASSERT_FALSE(parse("{\"t\":\"ping\"", &in));
    TEST_ASSERT_FALSE(parse("[\"t\",\"ping\"]", &in));
    TEST_ASSERT_FALSE(parse("{\"type\":\"ping\"}", &in));
    TEST_ASSERT_FALSE(parse("{\"t\":\"ping\"} {}", &in));
    TEST_ASSERT_FALSE(parse("", &in));
}

/* ---- the Z-Wave devices the panel switches (ZW-036, protocol.md "Z-Wave devices") ----------- */

/* The whole list, golden; a level not known is null; a garage carries its state. */
static void test_zwave_devices_golden(void)
{
    proto_zw_device_t d[2];
    memset(d, 0, sizeof d);
    snprintf(d[0].key, sizeof d[0].key, "zw_c0ffee01_4_1_0");
    snprintf(d[0].name, sizeof d[0].name, "Porch light");
    d[0].kind = PROTO_ZW_LIGHT;
    d[0].level = 40;
    d[0].online = true;
    snprintf(d[1].key, sizeof d[1].key, "zw_c0ffee01_6_1_1");
    snprintf(d[1].name, sizeof d[1].name, "Garage door");
    d[1].kind = PROTO_ZW_GARAGE;
    d[1].level = -1;
    snprintf(d[1].garage, sizeof d[1].garage, "closed");
    proto_link_zwave_devices(&W, d, 2u);
    TEST_ASSERT_EQUAL_STRING(
        "{\"t\":\"zwave_devices\",\"devices\":[{\"key\":\"zw_c0ffee01_4_1_0\",\"kind\":\"light\","
        "\"name\":\"Porch light\",\"level\":40,\"online\":true},{\"key\":\"zw_c0ffee01_6_1_1\","
        "\"kind\":\"garage\",\"name\":\"Garage door\",\"level\":null,\"online\":false,"
        "\"state\":\"closed\"}]}",
        done());
    pj_init(&W, BUF, sizeof BUF);
    proto_link_zwave_result(&W, "1a", false, "not_allowed");
    TEST_ASSERT_EQUAL_STRING(
        "{\"t\":\"zwave_result\",\"id\":\"1a\",\"ok\":false,\"error\":\"not_allowed\"}", done());
    pj_init(&W, BUF, sizeof BUF);
    proto_link_zwave_result(&W, "1b", true, "ignored");
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"zwave_result\",\"id\":\"1b\",\"ok\":true,\"error\":\"\"}",
                             done());
}

/* A command: its type recognised, its fields strict (an id, a key, a level 0..100). */
static void test_parse_zwave_set(void)
{
    static const char OK[] = "{\"t\":\"zwave_set\",\"id\":\"2c\",\"key\":\"zw_c0ffee01_4_1_0\","
                             "\"level\":55,\"later\":1}";
    proto_link_in_t in;
    TEST_ASSERT_TRUE(proto_link_parse(OK, strlen(OK), &in));
    TEST_ASSERT_EQUAL_INT(PROTO_LINK_IN_ZWAVE_SET, in.type);
    char id[PROTO_LINK_CALL_ID_LEN];
    char key[PROTO_ZW_KEY_LEN];
    int level = 0;
    TEST_ASSERT_TRUE(proto_link_parse_zwave_set(OK, strlen(OK), id, key, &level));
    TEST_ASSERT_EQUAL_STRING("2c", id);
    TEST_ASSERT_EQUAL_STRING("zw_c0ffee01_4_1_0", key);
    TEST_ASSERT_EQUAL_INT(55, level);
    static const char *const BAD[] = {
        "{\"t\":\"zwave_set\",\"key\":\"k\",\"level\":1}",
        "{\"t\":\"zwave_set\",\"id\":\"1\",\"level\":1}",
        "{\"t\":\"zwave_set\",\"id\":\"1\",\"key\":\"k\"}",
        "{\"t\":\"zwave_set\",\"id\":\"1\",\"key\":\"k\",\"level\":101}",
        "{\"t\":\"zwave_set\",\"id\":\"1\",\"key\":\"k\",\"level\":\"50\"}",
        "{\"t\":\"zwave_set\",\"id\":\"1\",\"key\":\"k\",\"level\":5",
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        TEST_ASSERT_FALSE_MESSAGE(proto_link_parse_zwave_set(BAD[i], strlen(BAD[i]), id, key, &level),
                                  BAD[i]);
    }
}

/* ---- panel settings and keypads (HAI-016..019, protocol.md "Panel settings and keypads") ---------
 * The golden strings below are the ones the integration's tests read (docs/features/23: the
 * aioroboalarms side of BL-216): change one, change both. */

static pj_writer_t BW; /* over BIG: the keypads list doesn't fit BUF */

static const char *big_done(void)
{
    TEST_ASSERT_TRUE_MESSAGE(pj_finish(&BW) > 0u, "document overflowed or isn't closed");
    return BIG;
}

/* panel_prefs_defaults() as fields (as test_prefs.c's). */
static proto_prefs_values_t prefs_defaults(void)
{
    proto_prefs_values_t v;
    memset(&v, 0, sizeof v);
    v.have = PROTO_PREFS_KNOWN;
    v.brightness = 80u;
    v.saver_brightness = 25u;
    v.saver_after_ms = 60000u;
    v.off_after_ms = 0u;
    v.night_on = false;
    v.night_start = 22u * 60u;
    v.night_end = 7u * 60u;
    v.night_brightness = 20u;
    v.night_off_after_ms = 60000u;
    v.clock_mode = 0u;
    v.appearance = 0u;
    v.high_contrast = false;
    v.volume = 70u;
    v.chime_volume = 70u;
    v.voice_volume = 70u;
    v.key_click = true;
    v.voice_enabled = true;
    v.voice_status = true;
    v.voice_chime = true;
    return v;
}

#define DEFAULTS_JSON                                                                              \
    "{\"brightness\":80,\"saver_level\":25,\"saver_after\":60,\"off_after\":0,\"night\":false,"    \
    "\"night_start\":1320,\"night_end\":420,\"night_level\":20,\"night_off\":60,\"clock\":0,"      \
    "\"appearance\":0,\"contrast\":false,\"beeps\":70,\"chime_vol\":70,\"voice_vol\":70,"          \
    "\"key_click\":true,\"voice\":true,\"voice_stat\":true,\"voice_chime\":true}"
#define ALL_KEYS_JSON                                                                              \
    "[\"brightness\",\"saver_level\",\"saver_after\",\"off_after\",\"night\",\"night_start\","     \
    "\"night_end\",\"night_level\",\"night_off\",\"clock\",\"appearance\",\"contrast\",\"beeps\"," \
    "\"chime_vol\",\"voice_vol\",\"key_click\",\"voice\",\"voice_stat\",\"voice_chime\"]"

/* HAI-016, HAI-017: the master's own settings, whole: every value, the keys it takes, its voice. */
static void test_settings_golden(void)
{
    pj_init(&BW, BIG, sizeof BIG);
    const proto_prefs_values_t v = prefs_defaults();
    proto_link_settings(&BW, &v, PROTO_PREFS_KNOWN, PROTO_LINK_VOICE_READY);
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"settings\",\"values\":" DEFAULTS_JSON
                             ",\"can\":" ALL_KEYS_JSON ",\"voice\":\"ready\"}",
                             big_done());
    TEST_ASSERT_TRUE(strlen(BIG) < PROTO_PAYLOAD_MAX); /* the link task's ordinary buffer */

    /* the voice's three words, and null for one not known (0 or a later number) */
    proto_prefs_values_t one;
    memset(&one, 0, sizeof one);
    TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_NONE, proto_prefs_put(&one, PROTO_PREFS_F_BRIGHTNESS, 40u));
    static const struct {
        uint8_t voice;
        const char *json;
    } V[] = {
        {PROTO_LINK_VOICE_LOADING, "\"loading\""},
        {PROTO_LINK_VOICE_NONE, "\"none\""},
        {PROTO_LINK_VOICE_UNKNOWN, "null"},
        {4u, "null"},
    };
    for (size_t i = 0; i < sizeof V / sizeof V[0]; i++) {
        char want[128];
        snprintf(want, sizeof want,
                 "{\"t\":\"settings\",\"values\":{\"brightness\":40},\"can\":[\"brightness\","
                 "\"beeps\"],\"voice\":%s}",
                 V[i].json);
        pj_init(&W, BUF, sizeof BUF);
        /* an unknown bit in can names nothing */
        proto_link_settings(&W, &one, PROTO_PREFS_BIT(1u) | PROTO_PREFS_BIT(13u) | 0x80000000u,
                            V[i].voice);
        TEST_ASSERT_EQUAL_STRING(want, done());
    }
}

static proto_link_keypad_t KP[PROTO_LINK_KEYPADS_MAX];

static void keypad(proto_link_keypad_t *k, uint8_t device, const char *id, const char *name)
{
    memset(k, 0, sizeof *k);
    k->device = device;
    snprintf(k->id, sizeof k->id, "%s", id);
    snprintf(k->name, sizeof k->name, "%s", name);
}

/* HAI-016, HAI-018: the whole list; a keypad that reported and one that hasn't (null id, firmware,
 * signal, settings and voice; no keys), and the empty list of a master without keypads. */
static void test_keypads_golden(void)
{
    keypad(&KP[0], 1u, "c0ffee", "Hall");
    KP[0].home = 1u;
    KP[0].online = true;
    snprintf(KP[0].fw, sizeof KP[0].fw, "0.9.0");
    KP[0].rssi = -61;
    KP[0].reported = true;
    KP[0].settings = prefs_defaults();
    KP[0].settings.brightness = 60u;
    KP[0].settings.appearance = 1u;
    KP[0].can = PROTO_PREFS_KNOWN;
    KP[0].voice = PROTO_LINK_VOICE_NONE;
    keypad(&KP[1], 3u, "", "Bedroom");
    KP[1].home = 2u;
    KP[1].rssi = 0;                    /* not known */
    KP[1].settings = prefs_defaults(); /* not reported: never shown */
    KP[1].can = PROTO_PREFS_KNOWN;
    KP[1].voice = PROTO_LINK_VOICE_READY;
    pj_init(&BW, BIG, sizeof BIG);
    proto_link_keypads(&BW, KP, 2u);
    TEST_ASSERT_EQUAL_STRING(
        "{\"t\":\"keypads\",\"keypads\":[{\"device\":1,\"id\":\"c0ffee\",\"name\":\"Hall\",\"home\":1,"
        "\"online\":true,\"fw\":\"0.9.0\",\"rssi\":-61,\"settings\":{\"brightness\":60,"
        "\"saver_level\":25,\"saver_after\":60,\"off_after\":0,\"night\":false,"
        "\"night_start\":1320,\"night_end\":420,\"night_level\":20,\"night_off\":60,\"clock\":0,"
        "\"appearance\":1,\"contrast\":false,\"beeps\":70,\"chime_vol\":70,\"voice_vol\":70,"
        "\"key_click\":true,\"voice\":true,\"voice_stat\":true,\"voice_chime\":true},"
        "\"can\":" ALL_KEYS_JSON ",\"voice\":\"none\"},{\"device\":3,\"id\":null,"
        "\"name\":\"Bedroom\",\"home\":2,\"online\":false,\"fw\":null,\"rssi\":null,"
        "\"settings\":null,\"can\":[],\"voice\":null}]}",
        big_done());

    pj_init(&W, BUF, sizeof BUF);
    proto_link_keypads(&W, NULL, 0u);
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"keypads\",\"keypads\":[]}", done());
    pj_init(&W, BUF, sizeof BUF);
    proto_link_keypads(&W, KP, 0u);
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"keypads\",\"keypads\":[]}", done());
}

/* HAI-018: eight keypads at their longest (every field, the longest values and words, a name and
 * a firmware of quotes and backslashes the writer escapes) fit PROTO_LINK_KEYPADS_JSON_MAX. */
static void test_keypads_fit(void)
{
    for (uint8_t i = 0; i < PROTO_LINK_KEYPADS_MAX; i++) {
        proto_link_keypad_t *k = &KP[i];
        memset(k, 0, sizeof *k);
        k->device = (uint8_t)(i + 1u);
        snprintf(k->id, sizeof k->id, "abcde%u", (unsigned)i);
        memset(k->name, '"', sizeof k->name - 1u);
        memset(k->fw, '\\', sizeof k->fw - 1u);
        k->home = 8u;
        k->rssi = -128;
        k->reported = true;
        k->can = PROTO_PREFS_KNOWN;
        k->voice = PROTO_LINK_VOICE_LOADING;
        proto_prefs_values_t *v = &k->settings;
        v->have = PROTO_PREFS_KNOWN;
        v->brightness = 100u;
        v->saver_brightness = 50u;
        v->saver_after_ms = 86400000u;
        v->off_after_ms = 86400000u;
        v->night_off_after_ms = 86400000u;
        v->night_start = 1439u;
        v->night_end = 1439u;
        v->night_brightness = 100u;
        v->volume = 100u;
        v->chime_volume = 100u;
        v->voice_volume = 100u;
        v->clock_mode = 2u;
        v->appearance = 2u;
    }
    pj_writer_t w;
    pj_init(&w, BIG, PROTO_LINK_KEYPADS_JSON_MAX);
    proto_link_keypads(&w, KP, PROTO_LINK_KEYPADS_MAX);
    const size_t len = pj_finish(&w);
    TEST_ASSERT_TRUE_MESSAGE(len > 0u, "8 keypads overflow PROTO_LINK_KEYPADS_JSON_MAX");
    TEST_ASSERT_TRUE(valid_json(BIG));
}

/* HAI-018 (docs/features/11 "Settings over the link"): only a well-formed id is published; a twin
 * of a lower device number's, or the master's own, is null, and so never a target. */
static void test_keypads_tidy(void)
{
    keypad(&KP[0], 5u, "c0ffee", "A");
    keypad(&KP[1], 2u, "c0ffee", "B"); /* the lower number keeps it */
    keypad(&KP[2], 7u, "c0ffee", "C");
    keypad(&KP[3], 1u, "C0FFEE", "D");   /* uppercase: not one the link keeps */
    keypad(&KP[4], 3u, "a1b2c3", "E");   /* the master's own */
    keypad(&KP[5], 4u, "beef", "F");     /* too short */
    keypad(&KP[6], 6u, "", "G");         /* none yet: not counted */
    keypad(&KP[7], 8u, "0badf00d", "H"); /* too long */
    TEST_ASSERT_EQUAL_size_t(6u, proto_link_keypads_tidy(KP, 8u, "a1b2c3"));
    TEST_ASSERT_EQUAL_STRING("", KP[0].id);
    TEST_ASSERT_EQUAL_STRING("c0ffee", KP[1].id);
    TEST_ASSERT_EQUAL_STRING("", KP[2].id);
    TEST_ASSERT_EQUAL_STRING("", KP[3].id);
    TEST_ASSERT_EQUAL_STRING("", KP[4].id);
    TEST_ASSERT_EQUAL_STRING("", KP[5].id);
    TEST_ASSERT_EQUAL_STRING("", KP[6].id);
    TEST_ASSERT_EQUAL_STRING("", KP[7].id);

    /* unique, well-formed ids stay; the master's own may be NULL */
    keypad(&KP[0], 1u, "0a1b2c", "A");
    keypad(&KP[1], 2u, "3d4e5f", "B");
    TEST_ASSERT_EQUAL_size_t(0u, proto_link_keypads_tidy(KP, 2u, NULL));
    TEST_ASSERT_EQUAL_STRING("0a1b2c", KP[0].id);
    TEST_ASSERT_EQUAL_STRING("3d4e5f", KP[1].id);
    TEST_ASSERT_EQUAL_size_t(0u, proto_link_keypads_tidy(NULL, 0u, NULL));
}

static bool parse_set(const char *json, proto_link_settings_set_t *out)
{
    return proto_link_parse_settings_set(json, strlen(json), out);
}

/* HAI-017: a settings_set is recognised, read in any member order, its values taken against every
 * known key; unknown members ride along; an overlong target matches no panel. */
static void test_parse_settings_set(void)
{
    static const char OK[] = "{\"t\":\"settings_set\",\"id\":\"3f9a1c2e\",\"later\":{\"x\":[1]},"
                             "\"values\":{\"brightness\":40,\"saver_after\":600,\"night\":true,"
                             "\"beeps\":101,\"glow\":3},\"target\":\"a1b2c3\"}";
    proto_link_in_t in;
    TEST_ASSERT_TRUE(proto_link_parse(OK, strlen(OK), &in));
    TEST_ASSERT_EQUAL_INT(PROTO_LINK_IN_SETTINGS_SET, in.type);
    proto_link_settings_set_t s;
    TEST_ASSERT_TRUE(parse_set(OK, &s));
    TEST_ASSERT_FALSE(s.bad);
    TEST_ASSERT_EQUAL_STRING("3f9a1c2e", s.id);
    TEST_ASSERT_EQUAL_STRING("a1b2c3", s.target);
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_BIT(1u) | PROTO_PREFS_BIT(3u) | PROTO_PREFS_BIT(5u),
                            s.p.values.have);
    TEST_ASSERT_EQUAL_UINT8(40u, s.p.values.brightness);
    TEST_ASSERT_EQUAL_UINT32(600000u, s.p.values.saver_after_ms);
    TEST_ASSERT_TRUE(s.p.values.night_on);
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_BIT(13u), s.p.refused); /* beeps 101: that key alone */
    TEST_ASSERT_EQUAL_UINT8(PROTO_PREFS_WHY_RANGE, s.p.why[12]);
    TEST_ASSERT_EQUAL_UINT8(1u, s.p.unknown_n);
    TEST_ASSERT_EQUAL_STRING("glow", s.p.unknown[0]);

    /* the longest id echoed (11 bytes); a target longer than any panel id is "", found nowhere */
    TEST_ASSERT_TRUE(parse_set("{\"id\":\"0123456789a\",\"target\":\"0123456789abcdef0\","
                               "\"values\":{}}",
                               &s));
    TEST_ASSERT_FALSE(s.bad);
    TEST_ASSERT_EQUAL_STRING("0123456789a", s.id);
    TEST_ASSERT_EQUAL_STRING("", s.target);
    TEST_ASSERT_EQUAL_HEX32(0u, s.p.values.have);

    /* not a message (the connection closes, as for any message missing a required key) */
    static const char *const NOT[] = {
        "{\"t\":\"settings_set\",\"target\":\"a1b2c3\",\"values\":{}}",
        "{\"t\":\"settings_set\",\"id\":\"\",\"target\":\"a1b2c3\",\"values\":{}}",
        "{\"t\":\"settings_set\",\"id\":\"0123456789ab\",\"target\":\"a1b2c3\",\"values\":{}}",
        "{\"t\":\"settings_set\",\"id\":7,\"target\":\"a1b2c3\",\"values\":{}}",
        "{\"t\":\"settings_set\",\"id\":\"1\",\"values\":{}}",
        "{\"t\":\"settings_set\",\"id\":\"1\",\"target\":null,\"values\":{}}",
        "{\"t\":\"settings_set\",\"id\":\"1\",\"target\":7,\"values\":{}}",
        "{\"t\":\"settings_set\",\"id\":\"1\",\"target\":\"a1b2c3\",\"values\":{\"beeps\":1}",
        "{\"t\":\"settings_set\",\"id\":\"1\",\"target\":\"a1b2c3\",\"values\":{\"beeps\":1}} x",
        "{\"t\":\"settings_set\",\"id\":\"1\",\"target\":\"a1b2c3\",\"values\":{\"beeps\":}}",
        "[\"settings_set\"]",
        "",
    };
    for (size_t i = 0; i < sizeof NOT / sizeof NOT[0]; i++) {
        TEST_ASSERT_FALSE_MESSAGE(parse_set(NOT[i], &s), NOT[i]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("", s.id, NOT[i]);
    }

    /* HAI-017: the whole request is "bad", nothing of it taken; its id and target still read */
    static const char *const BAD[] = {
        "{\"t\":\"settings_set\",\"id\":\"9\",\"target\":\"a1b2c3\"}",
        "{\"t\":\"settings_set\",\"id\":\"9\",\"target\":\"a1b2c3\",\"values\":[1,{\"a\":2}]}",
        "{\"t\":\"settings_set\",\"id\":\"9\",\"target\":\"a1b2c3\",\"values\":\"beeps\"}",
        "{\"t\":\"settings_set\",\"id\":\"9\",\"target\":\"a1b2c3\",\"values\":null}",
        "{\"t\":\"settings_set\",\"id\":\"9\",\"target\":\"a1b2c3\",\"values\":{\"night\":1}}",
        "{\"t\":\"settings_set\",\"id\":\"9\",\"target\":\"a1b2c3\",\"values\":{\"beeps\":1.5}}",
        "{\"t\":\"settings_set\",\"id\":\"9\",\"target\":\"a1b2c3\",\"values\":{\"beeps\":null}}",
        "{\"t\":\"settings_set\",\"id\":\"9\",\"target\":\"a1b2c3\",\"values\":{\"beeps\":\"5\"}}",
        "{\"t\":\"settings_set\",\"id\":\"9\",\"target\":\"a1b2c3\","
        "\"values\":{\"beeps\":99999999999999999999}}",
        "{\"t\":\"settings_set\",\"id\":\"9\",\"target\":\"a1b2c3\","
        "\"values\":{\"beeps\":1,\"beeps\":2}}",
        "{\"t\":\"settings_set\",\"id\":\"9\",\"target\":\"a1b2c3\",\"values\":{\"\":1}}",
        "{\"t\":\"settings_set\",\"id\":\"9\",\"target\":\"a1b2c3\","
        "\"values\":{\"a_key_too_long\":1}}",
        "{\"t\":\"settings_set\",\"id\":\"9\",\"target\":\"a1b2c3\",\"values\":{\"beeps\":1},"
        "\"values\":{\"beeps\":2}}",
        "{\"t\":\"settings_set\",\"values\":{\"beeps\":1,\"night\":\"on\"},\"id\":\"9\","
        "\"target\":\"a1b2c3\"}",
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        TEST_ASSERT_TRUE_MESSAGE(parse_set(BAD[i], &s), BAD[i]);
        TEST_ASSERT_TRUE_MESSAGE(s.bad, BAD[i]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("9", s.id, BAD[i]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("a1b2c3", s.target, BAD[i]);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(0u, s.p.values.have, BAD[i]);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(0u, s.p.refused, BAD[i]);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, s.p.unknown_n, BAD[i]);
    }
}

/* HAI-017: the target takes only its own keys; a key it doesn't take is refused not_allowed,
 * whatever its value (out of range too), and never sent. */
static void test_settings_set_accept(void)
{
    proto_link_settings_set_t s;
    TEST_ASSERT_TRUE(parse_set("{\"id\":\"1\",\"target\":\"c0ffee\",\"values\":{\"brightness\":40,"
                               "\"beeps\":101,\"voice\":true,\"night_end\":0}}",
                               &s));
    const proto_prefs_mask_t can =
        PROTO_PREFS_KNOWN & ~(PROTO_PREFS_BIT(1u) | PROTO_PREFS_BIT(13u));
    proto_link_settings_set_accept(&s, can);
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_BIT(7u) | PROTO_PREFS_BIT(17u), s.p.values.have);
    TEST_ASSERT_TRUE(s.p.values.voice_enabled);
    TEST_ASSERT_EQUAL_UINT16(0u, s.p.values.night_end);
    TEST_ASSERT_EQUAL_UINT8(0u, s.p.values.brightness); /* dropped, not just unflagged */
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_BIT(1u) | PROTO_PREFS_BIT(13u), s.p.refused);
    TEST_ASSERT_EQUAL_UINT8(PROTO_PREFS_WHY_NOT_ALLOWED, s.p.why[0]);
    TEST_ASSERT_EQUAL_UINT8(PROTO_PREFS_WHY_NOT_ALLOWED, s.p.why[12]);

    /* a target that takes everything changes nothing; one that takes nothing refuses it all */
    TEST_ASSERT_TRUE(parse_set("{\"id\":\"1\",\"target\":\"c0ffee\",\"values\":{\"clock\":2}}", &s));
    proto_link_settings_set_accept(&s, PROTO_PREFS_KNOWN);
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_BIT(10u), s.p.values.have);
    TEST_ASSERT_EQUAL_UINT8(2u, s.p.values.clock_mode);
    proto_link_settings_set_accept(&s, 0u);
    TEST_ASSERT_EQUAL_HEX32(0u, s.p.values.have);
    TEST_ASSERT_EQUAL_UINT8(PROTO_PREFS_WHY_NOT_ALLOWED, s.p.why[9]);
}

/* HAI-017, HAI-019: settings_result - ok only when nothing is refused; the request's refusals,
 * the target's for the keys it was sent, and the unknown keys; a whole-request error names none. */
static void test_settings_result_golden(void)
{
    proto_link_settings_set_t s;
    /* protocol.md's example */
    TEST_ASSERT_TRUE(parse_set("{\"t\":\"settings_set\",\"id\":\"3f9a1c2e\",\"target\":\"a1b2c3\","
                               "\"values\":{\"brightness\":40}}",
                               &s));
    uint8_t why[PROTO_PREFS_F_LAST] = {0};
    why[0] = PROTO_PREFS_WHY_IN_USE;
    proto_link_settings_result(&W, &s, "", why);
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"settings_result\",\"id\":\"3f9a1c2e\",\"ok\":false,"
                             "\"error\":\"\",\"refused\":{\"brightness\":\"in_use\"}}",
                             done());
    pj_init(&W, BUF, sizeof BUF);
    memset(why, 0, sizeof why);
    proto_link_settings_result(&W, &s, NULL, why);
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"settings_result\",\"id\":\"3f9a1c2e\",\"ok\":true,"
                             "\"error\":\"\",\"refused\":{}}",
                             done());
    /* the six whole-request errors: nothing is named */
    static const char *const ERR[] = {
        PROTO_LINK_SET_E_NOT_FOUND, PROTO_LINK_SET_E_OFFLINE, PROTO_LINK_SET_E_UNSUPPORTED,
        PROTO_LINK_SET_E_BUSY,      PROTO_LINK_SET_E_TIMEOUT, PROTO_LINK_SET_E_BAD,
    };
    static const char *const WORD[] = {"not_found", "offline", "unsupported",
                                       "busy",      "timeout", "bad"};
    why[0] = PROTO_PREFS_WHY_HELD;
    for (size_t i = 0; i < sizeof ERR / sizeof ERR[0]; i++) {
        char want[160];
        snprintf(want, sizeof want,
                 "{\"t\":\"settings_result\",\"id\":\"3f9a1c2e\",\"ok\":false,\"error\":\"%s\","
                 "\"refused\":{}}",
                 WORD[i]);
        pj_init(&W, BUF, sizeof BUF);
        proto_link_settings_result(&W, &s, ERR[i], why);
        TEST_ASSERT_EQUAL_STRING(want, done());
    }

    /* everything at once, in id order, then the unknown keys as they came */
    TEST_ASSERT_TRUE(parse_set("{\"id\":\"7b\",\"target\":\"c0ffee\",\"values\":{\"zz_top\":1,"
                               "\"beeps\":101,\"voice\":true,\"glow\":3,\"night\":true,"
                               "\"appearance\":2,\"contrast\":true,\"clock\":1}}",
                               &s));
    proto_link_settings_set_accept(&s, PROTO_PREFS_KNOWN & ~PROTO_PREFS_BIT(11u));
    memset(why, 0, sizeof why);
    why[16] = PROTO_PREFS_WHY_VOICE_NOT_READY; /* voice */
    why[11] = PROTO_PREFS_WHY_BAD;             /* contrast: no word of its own */
    why[9] = 9u;                               /* clock: a number from a newer keypad */
    why[0] = PROTO_PREFS_WHY_BUSY;             /* brightness: not sent, so not named */
    why[12] = PROTO_PREFS_WHY_HELD;            /* beeps: its own refusal comes first */
    pj_init(&W, BUF, sizeof BUF);
    proto_link_settings_result(&W, &s, "", why);
    TEST_ASSERT_EQUAL_STRING(
        "{\"t\":\"settings_result\",\"id\":\"7b\",\"ok\":false,\"error\":\"\",\"refused\":"
        "{\"clock\":\"not_allowed\",\"appearance\":\"not_allowed\",\"contrast\":\"not_allowed\","
        "\"beeps\":\"out_of_range\",\"voice\":\"voice_not_ready\",\"zz_top\":\"not_allowed\","
        "\"glow\":\"not_allowed\"}}",
        done());

    /* the target's other words: held, busy */
    TEST_ASSERT_TRUE(parse_set("{\"id\":\"8c\",\"target\":\"c0ffee\",\"values\":{\"night\":true,"
                               "\"key_click\":false}}",
                               &s));
    memset(why, 0, sizeof why);
    why[4] = PROTO_PREFS_WHY_HELD;
    why[15] = PROTO_PREFS_WHY_BUSY;
    pj_init(&W, BUF, sizeof BUF);
    proto_link_settings_result(&W, &s, "", why);
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"settings_result\",\"id\":\"8c\",\"ok\":false,\"error\":\"\","
                             "\"refused\":{\"night\":\"held\",\"key_click\":\"busy\"}}",
                             done());

    /* an unknown key alone is a refusal; more than the parser names still make ok false */
    TEST_ASSERT_TRUE(parse_set("{\"id\":\"9d\",\"target\":\"c0ffee\",\"values\":{\"k1\":1,\"k2\":1,"
                               "\"k3\":1,\"k4\":1,\"k5\":1,\"k6\":1,\"k7\":1,\"k8\":1,\"k9\":1}}",
                               &s));
    TEST_ASSERT_EQUAL_UINT8(9u, s.p.unknown_n);
    pj_init(&W, BUF, sizeof BUF);
    proto_link_settings_result(&W, &s, "", NULL);
    TEST_ASSERT_EQUAL_STRING(
        "{\"t\":\"settings_result\",\"id\":\"9d\",\"ok\":false,\"error\":\"\",\"refused\":"
        "{\"k1\":\"not_allowed\",\"k2\":\"not_allowed\",\"k3\":\"not_allowed\","
        "\"k4\":\"not_allowed\",\"k5\":\"not_allowed\",\"k6\":\"not_allowed\","
        "\"k7\":\"not_allowed\",\"k8\":\"not_allowed\"}}",
        done());
}

/* The longest answer (every key refused with the longest word, 8 unknown keys of 11 bytes that
 * need escapes) fits the link task's ordinary buffer. */
static void test_settings_result_fits(void)
{
    proto_link_settings_set_t s;
    memset(&s, 0, sizeof s);
    memset(s.id, 'f', sizeof s.id - 1u);
    s.p.refused = PROTO_PREFS_KNOWN;
    memset(s.p.why, (int)PROTO_PREFS_WHY_VOICE_NOT_READY, sizeof s.p.why);
    s.p.unknown_n = 255u;
    for (unsigned u = 0; u < PROTO_PREFS_UNKNOWN_MAX; u++) {
        memset(s.p.unknown[u], '"', PROTO_PREFS_KEY_MAX);
        s.p.unknown[u][0] = (char)('a' + u);
    }
    pj_init(&BW, BIG, PROTO_PAYLOAD_MAX);
    proto_link_settings_result(&BW, &s, "", NULL);
    TEST_ASSERT_TRUE(valid_json(big_done()));
}

/* ---- settings_set's queue (HAI-019) ------------------------------------------------------- */

static proto_link_setq_t Q;
static proto_link_settings_set_t REQ;

_Static_assert(PROTO_LINK_SET_ANSWER_MS + 500u <= 7000u,
               "protocol.md: answered within 7 s, with the link task's 500 ms pass");
_Static_assert(PROTO_LINK_SET_ANSWER_MS > 6000u, "after a keypad's own 6 s give-up (LINK-051)");

static proto_link_set_slot_t *add(uint64_t now, uint8_t target)
{
    snprintf(REQ.id, sizeof REQ.id, "r%u", (unsigned)target);
    return proto_link_setq_add(&Q, now, target, &REQ);
}

/* At most one a second per target after a burst of 3, one in flight per target, targets apart. */
static void test_setq_turns(void)
{
    proto_link_setq_init(&Q);
    memset(&REQ, 0, sizeof REQ);
    proto_link_set_slot_t *a[5];
    for (size_t i = 0; i < 5u; i++) {
        a[i] = add(1000u, 0u);
        TEST_ASSERT_NOT_NULL(a[i]);
        TEST_ASSERT_EQUAL_UINT8(PROTO_LINK_SET_QUEUED, a[i]->state);
        TEST_ASSERT_EQUAL_STRING("r0", a[i]->req.id);
    }
    /* the burst, one in flight at a time, oldest first */
    for (size_t i = 0; i < 3u; i++) {
        proto_link_set_slot_t *s = proto_link_setq_next(&Q, 1000u);
        TEST_ASSERT_EQUAL_PTR(a[i], s);
        TEST_ASSERT_EQUAL_UINT8(PROTO_LINK_SET_FLIGHT, s->state);
        TEST_ASSERT_EQUAL_UINT64(1000u, s->sent_ms);
        TEST_ASSERT_NULL(proto_link_setq_next(&Q, 1000u)); /* its target has one in flight */
        const uint32_t ticket = s->ticket;
        TEST_ASSERT_EQUAL_PTR(s, proto_link_setq_find(&Q, ticket));
        proto_link_setq_free(&Q, s);
        TEST_ASSERT_EQUAL_UINT8(PROTO_LINK_SET_FREE, s->state);
        TEST_ASSERT_NULL(proto_link_setq_find(&Q, ticket)); /* answered: a copy comes late */
    }
    /* then one a second */
    TEST_ASSERT_NULL(proto_link_setq_next(&Q, 1999u));
    /* ... while another target has its own turns and its own one in flight */
    proto_link_set_slot_t *b = add(1500u, 8u);
    TEST_ASSERT_EQUAL_PTR(b, proto_link_setq_next(&Q, 1500u));
    TEST_ASSERT_EQUAL_PTR(a[3], proto_link_setq_next(&Q, 2000u));
    proto_link_setq_free(&Q, a[3]);
    TEST_ASSERT_NULL(proto_link_setq_next(&Q, 2999u));
    TEST_ASSERT_EQUAL_PTR(a[4], proto_link_setq_next(&Q, 3000u));
    proto_link_setq_free(&Q, a[4]);
    proto_link_setq_free(&Q, b);
    /* a quiet target earns its burst back: 3 at once again */
    for (size_t i = 0; i < 4u; i++) {
        a[i] = add(7000u, 0u);
    }
    for (size_t i = 0; i < 3u; i++) {
        proto_link_set_slot_t *s = proto_link_setq_next(&Q, 7000u);
        TEST_ASSERT_EQUAL_PTR(a[i], s);
        proto_link_setq_free(&Q, s);
    }
    TEST_ASSERT_NULL(proto_link_setq_next(&Q, 7000u));
    TEST_ASSERT_EQUAL_PTR(a[3], proto_link_setq_next(&Q, 8000u));

    /* out of range: no target 9; no request */
    TEST_ASSERT_NULL(add(8000u, (uint8_t)PROTO_LINK_SET_TARGETS));
    TEST_ASSERT_NULL(proto_link_setq_add(&Q, 8000u, 1u, NULL));
}

/* A queue of 8 over all targets; in arrival order per target, whatever the slot. */
static void test_setq_queue(void)
{
    proto_link_setq_init(&Q);
    memset(&REQ, 0, sizeof REQ);
    proto_link_set_slot_t *s[PROTO_LINK_SET_QUEUE];
    for (size_t i = 0; i < PROTO_LINK_SET_QUEUE; i++) {
        s[i] = add(100u, (uint8_t)(i % 2u));
        TEST_ASSERT_NOT_NULL(s[i]);
    }
    TEST_ASSERT_NULL(add(100u, 5u)); /* full: busy at once */
    proto_link_setq_free(&Q, s[2]);  /* a freed slot is used again, by the newest */
    proto_link_set_slot_t *late = add(100u, 0u);
    TEST_ASSERT_EQUAL_PTR(s[2], late);
    /* each target's burst of 3, oldest first across both */
    const proto_link_set_slot_t *want[] = {s[0], s[1], s[3], s[4], s[5], s[6]};
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        proto_link_set_slot_t *n = proto_link_setq_next(&Q, 100u);
        TEST_ASSERT_EQUAL_PTR(want[i], n);
        proto_link_setq_free(&Q, n);
    }
    TEST_ASSERT_NULL(proto_link_setq_next(&Q, 100u)); /* both used their burst */
    proto_link_set_slot_t *n = proto_link_setq_next(&Q, 1100u);
    TEST_ASSERT_EQUAL_PTR(s[7], n); /* target 1's 4th came before target 0's late one */
    proto_link_setq_free(&Q, n);
    TEST_ASSERT_EQUAL_PTR(late, proto_link_setq_next(&Q, 1100u));
}

/* Every request answered within 7 s: one never sent is busy, one sent and unanswered timeout,
 * oldest first; a reconnect drops the requests and keeps the turns and the tickets. */
static void test_setq_deadlines(void)
{
    proto_link_setq_init(&Q);
    memset(&REQ, 0, sizeof REQ);
    proto_link_set_slot_t *s[PROTO_LINK_SET_QUEUE];
    for (size_t i = 0; i < PROTO_LINK_SET_QUEUE; i++) {
        s[i] = add(1000u + i, 3u);
    }
    proto_link_set_slot_t *f = proto_link_setq_next(&Q, 1000u);
    TEST_ASSERT_EQUAL_PTR(s[0], f); /* in flight, and it never answers */
    TEST_ASSERT_NULL(proto_link_setq_expired(&Q, 1000u + PROTO_LINK_SET_ANSWER_MS - 1u));
    const uint64_t at = 1000u + PROTO_LINK_SET_ANSWER_MS + 10u;
    for (size_t i = 0; i < PROTO_LINK_SET_QUEUE; i++) {
        proto_link_set_slot_t *e = proto_link_setq_expired(&Q, at);
        TEST_ASSERT_EQUAL_PTR(s[i], e);
        TEST_ASSERT_EQUAL_UINT8(i == 0u ? PROTO_LINK_SET_FLIGHT : PROTO_LINK_SET_QUEUED, e->state);
        proto_link_setq_free(&Q, e);
    }
    TEST_ASSERT_NULL(proto_link_setq_expired(&Q, at));

    /* eight to one target in one instant, each answered 100 ms after it goes: all within 7 s */
    proto_link_setq_init(&Q);
    for (size_t i = 0; i < PROTO_LINK_SET_QUEUE; i++) {
        s[i] = add(1000u, 0u);
    }
    size_t answered = 0;
    uint64_t last = 0;
    for (uint64_t now = 1000u; now <= 9000u; now += 50u) {
        for (size_t i = 0; i < PROTO_LINK_SET_QUEUE; i++) {
            if (Q.slot[i].state == PROTO_LINK_SET_FLIGHT && now >= Q.slot[i].sent_ms + 100u) {
                proto_link_setq_free(&Q, &Q.slot[i]);
                answered++;
                last = now;
            }
        }
        TEST_ASSERT_NULL(proto_link_setq_expired(&Q, now));
        (void)proto_link_setq_next(&Q, now);
    }
    TEST_ASSERT_EQUAL_size_t(PROTO_LINK_SET_QUEUE, answered);
    TEST_ASSERT_EQUAL_UINT64(6100u, last); /* 3 at once, then one a second */

    /* a new connection: nothing left to answer, no new burst, no ticket used again */
    proto_link_set_slot_t *g = NULL;
    for (size_t i = 0; i < 3u; i++) {
        add(20000u, 0u);
        g = proto_link_setq_next(&Q, 20000u);
        TEST_ASSERT_NOT_NULL(g);
        if (i < 2u) {
            proto_link_setq_free(&Q, g);
        }
    }
    const uint32_t gt = g->ticket; /* the third, still in flight */
    TEST_ASSERT_NOT_NULL(add(20000u, 0u));
    proto_link_setq_clear(&Q);
    TEST_ASSERT_NULL(proto_link_setq_find(&Q, gt)); /* its late answer matches nothing */
    TEST_ASSERT_NULL(proto_link_setq_expired(&Q, 99999u));
    TEST_ASSERT_NULL(proto_link_setq_next(&Q, 99999u));
    g = add(20000u, 0u);
    TEST_ASSERT_TRUE(g->ticket > gt + 1u);
    TEST_ASSERT_NULL(proto_link_setq_next(&Q, 20000u)); /* the burst isn't back */
    TEST_ASSERT_EQUAL_PTR(g, proto_link_setq_next(&Q, 21000u));
}

/* Tickets are never 0, across their wrap too, and keep the order there; find takes only one in
 * flight. */
static void test_setq_tickets(void)
{
    proto_link_setq_init(&Q);
    memset(&REQ, 0, sizeof REQ);
    Q.next_ticket = 0xFFFFFFFFu;
    proto_link_set_slot_t *a = add(0u, 2u);
    proto_link_set_slot_t *b = add(0u, 2u);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, a->ticket);
    TEST_ASSERT_EQUAL_HEX32(1u, b->ticket);
    TEST_ASSERT_NULL(proto_link_setq_find(&Q, a->ticket)); /* queued: no answer is due */
    TEST_ASSERT_NULL(proto_link_setq_find(&Q, 0u));
    TEST_ASSERT_EQUAL_PTR(a, proto_link_setq_next(&Q, 0u));
    TEST_ASSERT_EQUAL_PTR(a, proto_link_setq_find(&Q, 0xFFFFFFFFu));
    proto_link_setq_free(&Q, a);
    TEST_ASSERT_EQUAL_PTR(b, proto_link_setq_next(&Q, 0u));
    TEST_ASSERT_EQUAL_PTR(b, proto_link_setq_find(&Q, 1u));
    /* a queue never initialised still gives no 0 */
    memset(&Q, 0, sizeof Q);
    TEST_ASSERT_EQUAL_HEX32(1u, add(0u, 0u)->ticket);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sha256_vectors);
    RUN_TEST(test_hex);
    RUN_TEST(test_framing);
    RUN_TEST(test_pair_code_vectors);
    RUN_TEST(test_pair_code_binds_certificates);
    RUN_TEST(test_hello_golden);
    RUN_TEST(test_pair_nonce_golden);
    RUN_TEST(test_pair_result_golden);
    RUN_TEST(test_state_messages);
    RUN_TEST(test_snapshot_fits_full_panel);
    RUN_TEST(test_watch_golden);
    RUN_TEST(test_catalog_round_trip);
    RUN_TEST(test_catalog_pages_and_refusals);
    RUN_TEST(test_catalog_unknown_keys_and_overlong_id);
    RUN_TEST(test_parse_state_message);
    RUN_TEST(test_state_values);
    RUN_TEST(test_state_modes);
    RUN_TEST(test_catalog_values);
    RUN_TEST(test_state_supports);
    RUN_TEST(test_catalog_supports);
    RUN_TEST(test_state_since);
    RUN_TEST(test_state_hvac_action);
    RUN_TEST(test_new_keys_beside_an_unknown_one);
    RUN_TEST(test_parse_hello);
    RUN_TEST(test_parse_pairing);
    RUN_TEST(test_parse_shapes);
    RUN_TEST(test_zwave_devices_golden);
    RUN_TEST(test_parse_zwave_set);
    RUN_TEST(test_settings_golden);
    RUN_TEST(test_keypads_golden);
    RUN_TEST(test_keypads_fit);
    RUN_TEST(test_keypads_tidy);
    RUN_TEST(test_parse_settings_set);
    RUN_TEST(test_settings_set_accept);
    RUN_TEST(test_settings_result_golden);
    RUN_TEST(test_settings_result_fits);
    RUN_TEST(test_setq_turns);
    RUN_TEST(test_setq_queue);
    RUN_TEST(test_setq_deadlines);
    RUN_TEST(test_setq_tickets);
    return UNITY_END();
}
