/*
 * test_phone.c - direct mode and the panel's own push (direct-mode.md, push-api.md; DIR-NNN, PUSH-NNN).
 */
#include <string.h>

#include "alarm_proto/proto_phone.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static proto_phone_t table[PROTO_PHONE_MAX];

static void test_table(void)
{
    memset(table, 0, sizeof(table));
    proto_phone_t *a = proto_phone_free_slot(table);
    TEST_ASSERT_EQUAL_PTR(&table[0], a);
    a->used = 1u;
    a->user = 3u;
    memset(a->pub, 0x22, sizeof(a->pub));
    char id[PROTO_PHONE_ID_LEN];
    proto_phone_id(a->pub, id);
    TEST_ASSERT_EQUAL_size_t(16u, strlen(id));
    TEST_ASSERT_EQUAL_PTR(a, proto_phone_find(table, id));
    TEST_ASSERT_NULL(proto_phone_find(table, "0000000000000000"));
    /* DIR-004: changing or deleting the person ends their phones. */
    table[5].used = 1u;
    table[5].user = 3u;
    table[6].used = 1u;
    table[6].user = 4u;
    TEST_ASSERT_EQUAL_size_t(2u, proto_phone_remove_user(table, 3u));
    TEST_ASSERT_NULL(proto_phone_find(table, id));
    TEST_ASSERT_EQUAL_UINT8(1u, table[6].used);
    for (size_t i = 0; i < PROTO_PHONE_MAX; i++) {
        table[i].used = 1u;
    }
    TEST_ASSERT_NULL(proto_phone_free_slot(table));
}

/* DIR-001 */
static void test_window(void)
{
    uint8_t tok[PROTO_PHONE_TOKEN_LEN];
    uint8_t bad[PROTO_PHONE_TOKEN_LEN];
    memset(tok, 7, sizeof(tok));
    memset(bad, 8, sizeof(bad));
    proto_phone_window_t w;
    memset(&w, 0, sizeof(w));
    TEST_ASSERT_FALSE(proto_phone_window_use(&w, tok, 0u));

    proto_phone_window_open(&w, tok, 5u, 1000u);
    TEST_ASSERT_FALSE(proto_phone_window_use(&w, bad, 1001u));
    TEST_ASSERT_TRUE(proto_phone_window_use(&w, tok, 1002u));
    TEST_ASSERT_FALSE(proto_phone_window_use(&w, tok, 1003u)); /* once */

    proto_phone_window_open(&w, tok, 5u, 1000u);
    TEST_ASSERT_FALSE(proto_phone_window_use(&w, tok, 1000u + PROTO_PHONE_PAIR_MS));

    proto_phone_window_open(&w, tok, 5u, 1000u);
    for (unsigned i = 0; i < PROTO_PHONE_PAIR_TRIES; i++) {
        TEST_ASSERT_FALSE(proto_phone_window_use(&w, bad, 1001u));
    }
    TEST_ASSERT_FALSE(proto_phone_window_use(&w, tok, 1002u)); /* three wrong: closed */
}

static void test_uri_and_signed_bytes(void)
{
    uint8_t fp[32];
    uint8_t tok[PROTO_PHONE_TOKEN_LEN];
    memset(fp, 0xAB, sizeof(fp));
    memset(tok, 0, sizeof(tok));
    char s[256];
    TEST_ASSERT_TRUE(proto_phone_pair_uri("a1b2", "192.0.2.5,panel.local", fp, tok, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING_LEN("roboalarms://pair?id=a1b2&h=192.0.2.5,panel.local&fp=abab", s, 57);
    TEST_ASSERT_NOT_NULL(strstr(s, "&t=AAAAAAAAAAAAAAAAAAAAAA"));
    TEST_ASSERT_FALSE(proto_phone_pair_uri("a1b2", "h", fp, tok, s, 40u));

    uint8_t out[PROTO_PHONE_SIGNED_MAX];
    size_t n = proto_phone_session_bytes("pid", "chal", out, sizeof(out));
    static const char W1[] = "roboalarms-phone-v1\0pid\0chal";
    TEST_ASSERT_EQUAL_size_t(sizeof(W1) - 1u, n);
    TEST_ASSERT_EQUAL_MEMORY(W1, out, n);
    n = proto_phone_action_bytes("pid", "chal", "disarm", out, sizeof(out));
    static const char W2[] = "roboalarms-phone-action-v1\0pid\0chal\0disarm";
    TEST_ASSERT_EQUAL_size_t(sizeof(W2) - 1u, n);
    TEST_ASSERT_EQUAL_MEMORY(W2, out, n);
    TEST_ASSERT_EQUAL_size_t(0u, proto_phone_session_bytes("pid", "chal", out, 4u));
}

/* DIR-003 */
static void test_guarded(void)
{
    TEST_ASSERT_TRUE(proto_phone_guarded("POST", "partition", "disarm"));
    TEST_ASSERT_TRUE(proto_phone_guarded("POST", "partition", "acknowledge"));
    TEST_ASSERT_FALSE(proto_phone_guarded("POST", "partition", "arm"));
    TEST_ASSERT_TRUE(proto_phone_guarded("POST", "people", NULL));
    TEST_ASSERT_TRUE(proto_phone_guarded("POST", "config/alarm", NULL));
    TEST_ASSERT_FALSE(proto_phone_guarded("GET", "people", NULL));
    TEST_ASSERT_FALSE(proto_phone_guarded("POST", "config/display", NULL));
}

/* PUSH-002: the handle and level are bound, so the relay can't move or upgrade a message. */
static void test_push_aad_and_plain(void)
{
    char s[256];
    TEST_ASSERT_EQUAL_size_t(strlen("ra-push-v1|ph1.h|alarm"),
                             proto_push_aad("ph1.h", "alarm", s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("ra-push-v1|ph1.h|alarm", s);
    const proto_push_plain_t p = {.title = "Burglary alarm", .body = "Front door",
                                  .loc = "p1", .kind = "alarm", .ref = "evt_1",
                                  .at = "2026-11-02T01:03:12Z", .category = "alarm",
                                  .thread = "p1", .sound = "alarm"};
    char buf[512];
    pj_writer_t w;
    pj_init(&w, buf, sizeof(buf));
    proto_push_plain(&w, &p);
    TEST_ASSERT_NOT_EQUAL_size_t(0u, pj_finish(&w));
    TEST_ASSERT_EQUAL_STRING("{\"v\":1,\"title\":\"Burglary alarm\",\"body\":\"Front door\","
                             "\"loc\":\"p1\",\"loc_name\":null,\"kind\":\"alarm\",\"ref\":\"evt_1\","
                             "\"at\":\"2026-11-02T01:03:12Z\",\"category\":\"alarm\","
                             "\"thread\":\"p1\",\"sound\":\"alarm\"}",
                             buf);
}

/* RFC 9421 by hand: the same lines the relay rebuilds (PUSH-009). */
static void test_signature_base(void)
{
    char s[64];
    TEST_ASSERT_EQUAL_size_t(8u, proto_push_b64((const uint8_t *)"foob", 4u, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("Zm9vYg==", s);
    TEST_ASSERT_EQUAL_size_t(4u, proto_push_b64((const uint8_t *)"foo", 3u, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("Zm9v", s);

    char digest[64];
    TEST_ASSERT_TRUE(proto_push_digest("abc", 3u, digest, sizeof(digest)));
    /* SHA-256("abc"), RFC 6234 */
    TEST_ASSERT_EQUAL_STRING("sha-256=:ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0=:", digest);

    char params[128];
    TEST_ASSERT_TRUE(proto_push_sig_params(1700000000, "ps_c41", params, sizeof(params)));
    TEST_ASSERT_EQUAL_STRING("(\"@method\" \"@target-uri\" \"content-digest\");created=1700000000;"
                             "keyid=\"ps_c41\";alg=\"ed25519\"",
                             params);
    char base[400];
    TEST_ASSERT_NOT_EQUAL_size_t(0u, proto_push_sig_base("POST", "https://push.example.org/v1/send",
                                                         digest, params, base, sizeof(base)));
    TEST_ASSERT_EQUAL_STRING(
        "\"@method\": POST\n\"@target-uri\": https://push.example.org/v1/send\n"
        "\"content-digest\": sha-256=:ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0=:\n"
        "\"@signature-params\": (\"@method\" \"@target-uri\" \"content-digest\");"
        "created=1700000000;keyid=\"ps_c41\";alg=\"ed25519\"",
        base);
}

/* Security review (DIR-006): a phone hears its own user's partitions and the system, nothing else,
 * and nothing once its user can't have a phone. */
static void test_hears(void)
{
    TEST_ASSERT_TRUE(proto_phone_hears(ALARM_AUTH_STANDARD, 0x01u, 1u));
    TEST_ASSERT_TRUE(proto_phone_hears(ALARM_AUTH_STANDARD, 0x01u, 0u));
    TEST_ASSERT_FALSE(proto_phone_hears(ALARM_AUTH_STANDARD, 0x01u, 2u));
    TEST_ASSERT_TRUE(proto_phone_hears(ALARM_AUTH_MASTER, 0x03u, 2u));
    TEST_ASSERT_FALSE(proto_phone_hears(ALARM_AUTH_NONE, 0xFFu, 1u));
    TEST_ASSERT_FALSE(proto_phone_hears(ALARM_AUTH_ARM_ONLY, 0xFFu, 0u));
    TEST_ASSERT_FALSE(proto_phone_hears(ALARM_AUTH_DURESS, 0xFFu, 1u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hears);
    RUN_TEST(test_table);
    RUN_TEST(test_window);
    RUN_TEST(test_uri_and_signed_bytes);
    RUN_TEST(test_guarded);
    RUN_TEST(test_push_aad_and_plain);
    RUN_TEST(test_signature_base);
    return UNITY_END();
}
