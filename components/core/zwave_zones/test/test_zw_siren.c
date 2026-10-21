/*
 * test_zw_siren.c - Z-Wave sirens as the panel's bell (docs/features/16 OUT-016..OUT-019,
 * docs/features/36 sirens.md).
 */
#include <string.h>

#include "unity.h"
#include "zwave_zones/zw_siren.h"
#include "zws_fixture_v1.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- a fake radio ------------------------------------------------------------------------------ */

#define SENDS 64
typedef struct {
    uint16_t node;
    uint8_t endpoint;
    uint8_t cmd[8];
    size_t len;
    uint32_t tag;
} sent_t;

static struct {
    sent_t s[SENDS];
    size_t n;
    bool refuse;
    int trouble; /* -1 never told */
    int trouble_calls;
} F;

static bool f_send(void *ctx, uint16_t node, uint8_t endpoint, const uint8_t *cmd, size_t len,
                   uint32_t tag)
{
    (void)ctx;
    if (F.refuse) {
        return false;
    }
    if (F.n < SENDS && len <= sizeof F.s[0].cmd) {
        sent_t *s = &F.s[F.n++];
        s->node = node;
        s->endpoint = endpoint;
        memcpy(s->cmd, cmd, len);
        s->len = len;
        s->tag = tag;
    }
    return true;
}

static void f_trouble(void *ctx, bool on)
{
    (void)ctx;
    F.trouble = on ? 1 : 0;
    F.trouble_calls++;
}

static const zws_port_t PORT = {.send = f_send, .bell_trouble = f_trouble};
static zws_t Z;

static void start(void)
{
    memset(&F, 0, sizeof F);
    F.trouble = -1;
    zws_init(&Z, &PORT);
    zws_radio(&Z, 0u, true);
}

static const sent_t *last(void)
{
    TEST_ASSERT_TRUE(F.n > 0u);
    return &F.s[F.n - 1u];
}

static void expect_last(const uint8_t *cmd, size_t len)
{
    const sent_t *s = last();
    TEST_ASSERT_EQUAL_UINT(len, s->len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(cmd, s->cmd, len);
}

/* A Sound Switch siren in slot 0: node 7, partition 1, burglary and fire, tones 3 / 9 / CO 0. */
static void sound_siren(uint64_t now)
{
    zws_config_t c;
    zws_defaults(&c, 7u, 1u, 0u, ZWS_KIND_SOUND);
    c.tone_burglary = 3u;
    c.tone_fire = 9u;
    TEST_ASSERT_EQUAL(ZWS_OK, zws_set(&Z, now, 0u, &c));
}

static void deliver_last(uint64_t now)
{
    TEST_ASSERT_TRUE(zws_send_done(&Z, now, last()->tag, true));
}

/* ---- OUT-016: it follows the engine's bell ----------------------------------------------------- */

/* OUT-016: the tone per alarm kind (Tone Play Set 79 08 <tone>), fire wins, stop (tone 0) when the
 * bell goes off. */
static void test_follows_the_bell_with_a_tone_per_kind(void)
{
    start();
    sound_siren(1000u);
    TEST_ASSERT_EQUAL_UINT(0u, F.n); /* quiet: nothing sent */
    zws_output(&Z, 2000u, 1u, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    const uint8_t burglary[] = {0x79, 0x08, 0x03};
    expect_last(burglary, sizeof burglary);
    TEST_ASSERT_EQUAL_UINT16(7u, last()->node);
    TEST_ASSERT_EQUAL_HEX32(ZWS_TAG, last()->tag & ZWS_TAG_MASK);
    deliver_last(2100u);
    zws_output(&Z, 3000u, 1u, ALARM_OUT_BELL, ALARM_PAT_TEMPORAL3);
    const uint8_t fire[] = {0x79, 0x08, 0x09};
    expect_last(fire, sizeof fire);
    deliver_last(3100u);
    zws_output(&Z, 4000u, 1u, ALARM_OUT_BELL, ALARM_PAT_TEMPORAL4);
    expect_last(burglary, sizeof burglary); /* CO 0: the burglary tone */
    deliver_last(4100u);
    zws_output(&Z, 5000u, 1u, ALARM_OUT_BELL, ALARM_PAT_OFF);
    const uint8_t stop[] = {0x79, 0x08, 0x00};
    expect_last(stop, sizeof stop);
    deliver_last(5100u);
    const size_t n = F.n;
    zws_poll(&Z, 200000u);
    TEST_ASSERT_EQUAL_UINT(n, F.n); /* quiet and confirmed: nothing more */
    TEST_ASSERT_EQUAL_INT(0, F.trouble == 1 ? 1 : 0);
}

/* OUT-016: the device's default tone (FF) and the volume byte (Sound Switch v2); a strobe output
 * alone sounds nothing; the tone is sent at the siren's end point. */
static void test_default_tone_volume_and_endpoint(void)
{
    start();
    zws_config_t c;
    zws_defaults(&c, 9u, 2u, 3u, ZWS_KIND_SOUND);
    c.volume = 60u;
    TEST_ASSERT_EQUAL(ZWS_OK, zws_set(&Z, 0u, 1u, &c));
    zws_output(&Z, 10u, 1u, ALARM_OUT_STROBE, ALARM_PAT_STEADY);
    TEST_ASSERT_EQUAL_UINT(0u, F.n);
    zws_output(&Z, 20u, 1u, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    const uint8_t on[] = {0x79, 0x08, 0xFF, 60u};
    expect_last(on, sizeof on);
    TEST_ASSERT_EQUAL_UINT8(3u, last()->endpoint);
    deliver_last(30u);
    zws_output(&Z, 40u, 1u, ALARM_OUT_BELL, ALARM_PAT_OFF);
    const uint8_t stop[] = {0x79, 0x08, 0x00}; /* no volume with a stop */
    expect_last(stop, sizeof stop);
}

/* OUT-016: what it sounds for - its scope by alarm kind, and `also` (another partition's alarms,
 * never its chirps or its armed state). */
static void test_scope_and_also(void)
{
    start();
    zws_config_t c;
    zws_defaults(&c, 7u, 1u, 0u, ZWS_KIND_SOUND);
    c.fire = false; /* burglary only */
    c.also = 0x02u; /* and partition 2's alarms */
    c.squawk = true;
    c.tone_arm = 0x11u;
    TEST_ASSERT_EQUAL(ZWS_OK, zws_set(&Z, 0u, 0u, &c));
    zws_output(&Z, 10u, 1u, ALARM_OUT_BELL, ALARM_PAT_TEMPORAL3);
    TEST_ASSERT_EQUAL_UINT(0u, F.n); /* fire: not its */
    zws_output(&Z, 20u, 1u, ALARM_OUT_BELL, ALARM_PAT_OFF);
    zws_output(&Z, 30u, 3u, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    TEST_ASSERT_EQUAL_UINT(0u, F.n); /* partition 3: neither its own nor also */
    zws_output(&Z, 40u, 2u, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    const uint8_t on[] = {0x79, 0x08, 0xFF};
    expect_last(on, sizeof on);
    deliver_last(50u);
    zws_output(&Z, 60u, 2u, ALARM_OUT_BELL, ALARM_PAT_OFF);
    deliver_last(70u);
    const size_t n = F.n;
    zws_armed(&Z, 80u, 2u, true); /* `also` never chirps */
    TEST_ASSERT_EQUAL_UINT(n, F.n);
    /* ... nor does it refuse a test */
    TEST_ASSERT_EQUAL(ZWS_OK, zws_test(&Z, 90u, 0u));
}

/* OUT-016: a Binary Switch siren (no Sound Switch): Set FF while any of its sounds is wanted, 00
 * when off; its Reports are the siren's (not a plain output's); it never chirps. */
static void test_binary_switch_fallback(void)
{
    start();
    zws_config_t c;
    zws_defaults(&c, 12u, 1u, 0u, ZWS_KIND_BINARY);
    c.squawk = true;
    c.tone_arm = 1u;
    TEST_ASSERT_EQUAL(ZWS_OK, zws_set(&Z, 0u, 0u, &c));
    zws_armed(&Z, 5u, 1u, true);
    TEST_ASSERT_EQUAL_UINT(0u, F.n);
    zws_armed(&Z, 6u, 1u, false);
    zws_output(&Z, 10u, 1u, ALARM_OUT_BELL, ALARM_PAT_TEMPORAL3);
    const uint8_t on[] = {0x25, 0x01, 0xFF};
    expect_last(on, sizeof on);
    deliver_last(20u);
    const uint8_t report_on[] = {0x25, 0x03, 0xFF};
    TEST_ASSERT_TRUE(zws_received(&Z, 30u, 12u, 0u, report_on, sizeof report_on));
    TEST_ASSERT_FALSE(zws_received(&Z, 30u, 12u, 1u, report_on, sizeof report_on)); /* another
                                                                                      * end point */
    zws_output(&Z, 40u, 1u, ALARM_OUT_BELL, ALARM_PAT_OFF);
    const uint8_t off[] = {0x25, 0x01, 0x00};
    expect_last(off, sizeof off);
    /* keep-up asks with Switch Get */
    zws_output(&Z, 50u, 1u, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    deliver_last(60u);
    zws_poll(&Z, 60u + ZWS_CHECK_MS);
    const uint8_t get[] = {0x25, 0x02};
    expect_last(get, sizeof get);
}

/* ---- OUT-017: confirmed, retried, kept up ------------------------------------------------------ */

/* OUT-017: not delivered - again after ZWS_RETRY_MS; ZWS_SEND_TRIES failures are the bell trouble;
 * then every ZWS_RETRY_SLOW_MS while still wanted, until one is delivered (trouble cleared). */
static void test_confirm_retry_and_trouble(void)
{
    start();
    sound_siren(0u);
    zws_output(&Z, 1000u, 1u, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    TEST_ASSERT_EQUAL_UINT(1u, F.n);
    zws_send_done(&Z, 1100u, last()->tag, false);
    zws_poll(&Z, 1100u + ZWS_RETRY_MS - 1u);
    TEST_ASSERT_EQUAL_UINT(1u, F.n);
    TEST_ASSERT_EQUAL_UINT64(1100u + ZWS_RETRY_MS, zws_next_deadline(&Z, 1200u));
    zws_poll(&Z, 1100u + ZWS_RETRY_MS);
    TEST_ASSERT_EQUAL_UINT(2u, F.n);
    zws_send_done(&Z, 3200u, last()->tag, false);
    TEST_ASSERT_NOT_EQUAL(1, F.trouble);
    zws_poll(&Z, 3200u + ZWS_RETRY_MS);
    TEST_ASSERT_EQUAL_UINT(3u, F.n);
    zws_send_done(&Z, 5300u, last()->tag, false); /* the third failure */
    TEST_ASSERT_EQUAL_INT(1, F.trouble);
    zws_poll(&Z, 5300u + ZWS_RETRY_MS);
    TEST_ASSERT_EQUAL_UINT(3u, F.n); /* slowly now */
    zws_poll(&Z, 5300u + ZWS_RETRY_SLOW_MS);
    TEST_ASSERT_EQUAL_UINT(4u, F.n);
    const uint8_t on[] = {0x79, 0x08, 0x03};
    expect_last(on, sizeof on);
    deliver_last(15400u);
    TEST_ASSERT_EQUAL_INT(0, F.trouble);
    /* a late end of an older try changes nothing */
    TEST_ASSERT_TRUE(zws_send_done(&Z, 15500u, F.s[0].tag, false));
    TEST_ASSERT_EQUAL_INT(0, F.trouble);
    /* someone else's tag is not the sirens' */
    TEST_ASSERT_FALSE(zws_send_done(&Z, 15500u, 0x4F000001u, false));
}

/* OUT-017: a send the radio refuses (not queued) and one whose end never comes count as failed
 * tries too. */
static void test_refused_and_unanswered_sends(void)
{
    start();
    sound_siren(0u);
    F.refuse = true;
    zws_output(&Z, 1000u, 1u, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    zws_poll(&Z, 1000u + ZWS_RETRY_MS);
    zws_poll(&Z, 1000u + 2u * ZWS_RETRY_MS);
    TEST_ASSERT_EQUAL_INT(1, F.trouble);
    F.refuse = false;
    zws_poll(&Z, 1000u + 2u * ZWS_RETRY_MS + ZWS_RETRY_SLOW_MS);
    TEST_ASSERT_EQUAL_UINT(1u, F.n);
    /* no end ever told: after ZWS_CONFIRM_MS it is a failed try, the next goes slowly */
    const uint64_t t = 1000u + 2u * ZWS_RETRY_MS + ZWS_RETRY_SLOW_MS;
    TEST_ASSERT_EQUAL_UINT64(t + ZWS_CONFIRM_MS, zws_next_deadline(&Z, t));
    zws_poll(&Z, t + ZWS_CONFIRM_MS);
    zws_poll(&Z, t + ZWS_CONFIRM_MS + ZWS_RETRY_SLOW_MS);
    TEST_ASSERT_EQUAL_UINT(2u, F.n);
    deliver_last(t + ZWS_CONFIRM_MS + ZWS_RETRY_SLOW_MS + 10u);
    TEST_ASSERT_EQUAL_INT(0, F.trouble);
}

/* OUT-017: while it sounds it is asked every ZWS_CHECK_MS (Tone Play Get); a Report of tone 0
 * while its sound is wanted sends the tone again; a Report that it plays changes nothing. */
static void test_keep_up_while_sounding(void)
{
    start();
    sound_siren(0u);
    zws_output(&Z, 1000u, 1u, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    deliver_last(1100u);
    TEST_ASSERT_EQUAL_UINT64(1100u + ZWS_CHECK_MS, zws_next_deadline(&Z, 1200u));
    zws_poll(&Z, 1100u + ZWS_CHECK_MS);
    const uint8_t get[] = {0x79, 0x09};
    expect_last(get, sizeof get);
    TEST_ASSERT_TRUE(zws_send_done(&Z, 61200u, last()->tag, true));
    const uint8_t playing[] = {0x79, 0x0A, 0x03, 0x50};
    TEST_ASSERT_TRUE(zws_received(&Z, 61300u, 7u, 0u, playing, sizeof playing));
    const size_t n = F.n;
    zws_poll(&Z, 61400u);
    TEST_ASSERT_EQUAL_UINT(n, F.n);
    const uint8_t quiet[] = {0x79, 0x0A, 0x00};
    TEST_ASSERT_TRUE(zws_received(&Z, 61500u, 7u, 0u, quiet, sizeof quiet));
    TEST_ASSERT_EQUAL_UINT(n, F.n); /* nothing sent from the radio's own news */
    zws_poll(&Z, 61500u);
    const uint8_t on[] = {0x79, 0x08, 0x03};
    expect_last(on, sizeof on);
    /* quiet wanted: a quiet Report is simply right, and no more Gets */
    deliver_last(61600u);
    zws_output(&Z, 62000u, 1u, ALARM_OUT_BELL, ALARM_PAT_OFF);
    deliver_last(62100u);
    const size_t m = F.n;
    TEST_ASSERT_TRUE(zws_received(&Z, 62200u, 7u, 0u, quiet, sizeof quiet));
    zws_poll(&Z, 62200u + 2u * ZWS_CHECK_MS);
    TEST_ASSERT_EQUAL_UINT(m, F.n);
    /* another class's command is not the siren's */
    const uint8_t battery[] = {0x80, 0x03, 0x64};
    TEST_ASSERT_FALSE(zws_received(&Z, 62300u, 7u, 0u, battery, sizeof battery));
    /* nor a node that isn't one */
    TEST_ASSERT_FALSE(zws_received(&Z, 62300u, 8u, 0u, quiet, sizeof quiet));
}

/* OUT-017: a stop nobody takes is given up after ZWS_STOP_TRIES (the trouble stays) and sent
 * again once the siren is heard. */
static void test_stop_given_up_until_heard(void)
{
    start();
    sound_siren(0u);
    zws_output(&Z, 1000u, 1u, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    deliver_last(1100u);
    zws_output(&Z, 2000u, 1u, ALARM_OUT_BELL, ALARM_PAT_OFF);
    uint64_t t = 2000u;
    for (unsigned i = 0; i < ZWS_STOP_TRIES; i++) {
        zws_poll(&Z, t);
        zws_send_done(&Z, t, last()->tag, false);
        t += ZWS_RETRY_SLOW_MS;
    }
    TEST_ASSERT_EQUAL_INT(1, F.trouble);
    const size_t n = F.n;
    zws_poll(&Z, t + 10u * ZWS_RETRY_SLOW_MS);
    TEST_ASSERT_EQUAL_UINT(n, F.n);
    zws_heard(&Z, t + 10u * ZWS_RETRY_SLOW_MS, 7u);
    zws_poll(&Z, t + 10u * ZWS_RETRY_SLOW_MS);
    TEST_ASSERT_EQUAL_UINT(n + 1u, F.n);
    const uint8_t stop[] = {0x79, 0x08, 0x00};
    expect_last(stop, sizeof stop);
    deliver_last(t + 10u * ZWS_RETRY_SLOW_MS + 10u);
    TEST_ASSERT_EQUAL_INT(0, F.trouble);
}

/* ---- OUT-018: supervision ---------------------------------------------------------------------- */

/* OUT-018: unheard for its limit (240 min by default) is the bell trouble; any frame clears it;
 * while the radio is down silence doesn't count, and coming up it counts from then. */
static void test_supervision_silence(void)
{
    start();
    sound_siren(1000u);
    const uint64_t limit = (uint64_t)ZWS_SUPERVISION_DEF_MIN * 60000u;
    TEST_ASSERT_EQUAL_UINT64(1000u + limit, zws_next_deadline(&Z, 1000u));
    zws_poll(&Z, 1000u + limit - 1u);
    TEST_ASSERT_NOT_EQUAL(1, F.trouble);
    zws_poll(&Z, 1000u + limit);
    TEST_ASSERT_EQUAL_INT(1, F.trouble);
    zws_heard(&Z, 1000u + limit + 5u, 7u);
    TEST_ASSERT_EQUAL_INT(0, F.trouble);
    /* the radio down: no silence however long */
    zws_radio(&Z, 2000u + limit, false);
    zws_poll(&Z, 2000u + 3u * limit);
    TEST_ASSERT_EQUAL_INT(0, F.trouble);
    zws_radio(&Z, 2000u + 3u * limit, true);
    zws_poll(&Z, 2000u + 4u * limit - 1u);
    TEST_ASSERT_EQUAL_INT(0, F.trouble);
    zws_poll(&Z, 2000u + 4u * limit);
    TEST_ASSERT_EQUAL_INT(1, F.trouble);
    /* cleared, the trouble goes with it */
    TEST_ASSERT_EQUAL(ZWS_OK, zws_clear(&Z, 2000u + 4u * limit, 0u));
    TEST_ASSERT_EQUAL_INT(0, F.trouble);
}

/* OUT-018: the installer's own limit (at least ZWS_SUPERVISION_MIN_MIN), and the rules of an
 * assignment. */
static void test_supervision_limit_and_rules(void)
{
    start();
    zws_config_t c;
    zws_defaults(&c, 7u, 1u, 0u, ZWS_KIND_SOUND);
    c.supervision_min = 59u;
    TEST_ASSERT_EQUAL(ZWS_ERR_ARG, zws_set(&Z, 0u, 0u, &c));
    c.supervision_min = 60u;
    TEST_ASSERT_EQUAL(ZWS_OK, zws_set(&Z, 0u, 0u, &c));
    TEST_ASSERT_EQUAL_UINT64(60u * 60000u, zws_silent_limit_ms(&c));
    zws_poll(&Z, 60u * 60000u);
    TEST_ASSERT_EQUAL_INT(1, F.trouble);
    zws_config_t bad = c;
    bad.partitions = 0u;
    TEST_ASSERT_EQUAL(ZWS_ERR_ARG, zws_set(&Z, 0u, 1u, &bad));
    bad = c;
    bad.burglary = bad.fire = false;
    TEST_ASSERT_EQUAL(ZWS_ERR_ARG, zws_set(&Z, 0u, 1u, &bad));
    bad = c;
    bad.volume = 101u;
    TEST_ASSERT_EQUAL(ZWS_ERR_ARG, zws_set(&Z, 0u, 1u, &bad));
    bad = c;
    bad.kind = 3u;
    TEST_ASSERT_EQUAL(ZWS_ERR_ARG, zws_set(&Z, 0u, 1u, &bad));
    TEST_ASSERT_EQUAL(ZWS_ERR_ARG, zws_set(&Z, 0u, 1u, &c)); /* the same siren twice */
    TEST_ASSERT_EQUAL(ZWS_ERR_ARG, zws_set(&Z, 0u, ZWS_SIRENS, &c));
    TEST_ASSERT_EQUAL_INT(0, zws_slot_for(&Z, 7u, 0u));
    TEST_ASSERT_EQUAL_INT(1, zws_slot_for(&Z, 8u, 0u));
    /* its device left: the assignment goes, and its trouble */
    TEST_ASSERT_TRUE(zws_forget(&Z, 7u));
    TEST_ASSERT_EQUAL_INT(-1, zws_find(&Z, 7u, 0u));
    TEST_ASSERT_EQUAL_INT(0, F.trouble);
    TEST_ASSERT_FALSE(zws_forget(&Z, 7u));
}

/* ---- OUT-019: squawk, test, store --------------------------------------------------------------- */

/* OUT-019: the arm and disarm tones when its own partition arms and disarms - once, never
 * retried, not while it sounds; zws_set_armed says nothing; tone 0 chirps nothing. */
static void test_squawk(void)
{
    start();
    zws_config_t c;
    zws_defaults(&c, 7u, 1u, 0u, ZWS_KIND_SOUND);
    c.squawk = true;
    c.tone_arm = 0x11u;
    c.tone_disarm = 0x12u;
    c.volume = 30u;
    TEST_ASSERT_EQUAL(ZWS_OK, zws_set(&Z, 0u, 0u, &c));
    zws_set_armed(&Z, 1u, true); /* the start-up's view: no chirp */
    TEST_ASSERT_EQUAL_UINT(0u, F.n);
    zws_armed(&Z, 10u, 1u, true); /* no change: no chirp */
    TEST_ASSERT_EQUAL_UINT(0u, F.n);
    zws_armed(&Z, 20u, 1u, false);
    const uint8_t disarm[] = {0x79, 0x08, 0x12, 30u};
    expect_last(disarm, sizeof disarm);
    TEST_ASSERT_TRUE(zws_send_done(&Z, 30u, last()->tag, false)); /* never retried */
    zws_poll(&Z, 30u + ZWS_RETRY_SLOW_MS);
    TEST_ASSERT_EQUAL_UINT(1u, F.n);
    TEST_ASSERT_NOT_EQUAL(1, F.trouble);
    zws_armed(&Z, 40u, 1u, true);
    const uint8_t arm[] = {0x79, 0x08, 0x11, 30u};
    expect_last(arm, sizeof arm);
    /* sounding: no chirp */
    zws_output(&Z, 50u, 1u, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    deliver_last(60u);
    const size_t n = F.n;
    zws_armed(&Z, 70u, 1u, false);
    TEST_ASSERT_EQUAL_UINT(n, F.n);
    /* squawk off: nothing */
    zws_output(&Z, 80u, 1u, ALARM_OUT_BELL, ALARM_PAT_OFF);
    deliver_last(90u);
    c.squawk = false;
    TEST_ASSERT_EQUAL(ZWS_OK, zws_set(&Z, 100u, 0u, &c));
    const size_t m = F.n;
    zws_armed(&Z, 110u, 1u, true);
    TEST_ASSERT_EQUAL_UINT(m, F.n);
}

/* OUT-019: "Test it" plays its burglary tone ZWS_TEST_MS then stops; refused while one of its own
 * partitions is armed, while it sounds, while the radio is down, or for no siren. */
static void test_test_and_refusals(void)
{
    start();
    sound_siren(0u);
    TEST_ASSERT_EQUAL(ZWS_ERR_UNKNOWN, zws_test(&Z, 0u, 1u));
    zws_set_armed(&Z, 1u, true);
    TEST_ASSERT_EQUAL(ZWS_ERR_ARMED, zws_test(&Z, 10u, 0u));
    zws_set_armed(&Z, 1u, false);
    TEST_ASSERT_EQUAL(ZWS_OK, zws_test(&Z, 20u, 0u));
    const uint8_t on[] = {0x79, 0x08, 0x03};
    expect_last(on, sizeof on);
    deliver_last(30u);
    TEST_ASSERT_EQUAL(ZWS_ERR_BUSY, zws_test(&Z, 40u, 0u));
    TEST_ASSERT_EQUAL_UINT64(20u + ZWS_TEST_MS, zws_next_deadline(&Z, 50u));
    zws_poll(&Z, 20u + ZWS_TEST_MS);
    const uint8_t stop[] = {0x79, 0x08, 0x00};
    expect_last(stop, sizeof stop);
    deliver_last(20u + ZWS_TEST_MS + 5u);
    zws_output(&Z, 3000u, 1u, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    TEST_ASSERT_EQUAL(ZWS_ERR_BUSY, zws_test(&Z, 3010u, 0u));
    zws_output(&Z, 3020u, 1u, ALARM_OUT_BELL, ALARM_PAT_OFF);
    deliver_last(3030u);
    zws_radio(&Z, 3040u, false);
    TEST_ASSERT_EQUAL(ZWS_ERR_OFFLINE, zws_test(&Z, 3050u, 0u));
    /* the engine's own bell test (WALK-003) sounds it briefly too */
    zws_radio(&Z, 3060u, true);
    zws_output(&Z, 4000u, 1u, ALARM_OUT_BELL, ALARM_PAT_TEST);
    expect_last(on, sizeof on);
    deliver_last(4010u);
    zws_poll(&Z, 4000u + SIREN_TEST_MS);
    expect_last(stop, sizeof stop);
}

/* OUT-019: the stored blob - the frozen fixture reads as written and encodes back to itself;
 * shorter entries read missing fields as 0, longer ones skip the rest; damage refuses it all. */
static void test_store_and_fixture(void)
{
    zws_config_t got[ZWS_SIRENS];
    TEST_ASSERT_TRUE(zws_decode(ZWS_FIXTURE_V1, sizeof ZWS_FIXTURE_V1, got));
    TEST_ASSERT_FALSE(got[0].used);
    TEST_ASSERT_TRUE(got[1].used);
    TEST_ASSERT_EQUAL_UINT16(18u, got[1].node);
    TEST_ASSERT_EQUAL_UINT16(3u, got[1].generation);
    TEST_ASSERT_EQUAL_UINT8(2u, got[1].endpoint);
    TEST_ASSERT_EQUAL_UINT8(ZWS_KIND_SOUND, got[1].kind);
    TEST_ASSERT_EQUAL_HEX8(0x03u, got[1].partitions);
    TEST_ASSERT_EQUAL_HEX8(0x04u, got[1].also);
    TEST_ASSERT_TRUE(got[1].burglary);
    TEST_ASSERT_FALSE(got[1].fire);
    TEST_ASSERT_TRUE(got[1].squawk);
    TEST_ASSERT_EQUAL_UINT8(5u, got[1].tone_burglary);
    TEST_ASSERT_EQUAL_UINT8(10u, got[1].tone_fire);
    TEST_ASSERT_EQUAL_UINT8(0u, got[1].tone_co);
    TEST_ASSERT_EQUAL_UINT8(0x11u, got[1].tone_arm);
    TEST_ASSERT_EQUAL_UINT8(0x12u, got[1].tone_disarm);
    TEST_ASSERT_EQUAL_UINT8(80u, got[1].volume);
    TEST_ASSERT_EQUAL_UINT16(120u, got[1].supervision_min);
    TEST_ASSERT_FALSE(got[2].used);
    TEST_ASSERT_TRUE(got[3].used);
    TEST_ASSERT_EQUAL_UINT16(261u, got[3].node);
    TEST_ASSERT_EQUAL_UINT8(ZWS_KIND_BINARY, got[3].kind);
    TEST_ASSERT_TRUE(got[3].fire);
    TEST_ASSERT_FALSE(got[3].burglary);
    TEST_ASSERT_EQUAL_UINT16(0u, got[3].supervision_min);

    start();
    zws_load(&Z, got);
    uint8_t blob[ZWS_BLOB_MAX];
    const size_t n = zws_encode(&Z, blob, sizeof blob);
    TEST_ASSERT_EQUAL_UINT(sizeof ZWS_FIXTURE_V1, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ZWS_FIXTURE_V1, blob, n);
    TEST_ASSERT_EQUAL_UINT(0u, zws_encode(&Z, blob, sizeof blob - 1u));

    /* damage: a byte, the CRC, the magic, a format from the future, a cut */
    uint8_t bad[sizeof ZWS_FIXTURE_V1];
    memcpy(bad, ZWS_FIXTURE_V1, sizeof bad);
    bad[7] ^= 0x01u;
    zws_config_t keep[ZWS_SIRENS];
    memset(keep, 0xA5, sizeof keep);
    TEST_ASSERT_FALSE(zws_decode(bad, sizeof bad, keep));
    TEST_ASSERT_EQUAL_HEX8(0xA5u, ((const uint8_t *)keep)[0]); /* all or nothing */
    memcpy(bad, ZWS_FIXTURE_V1, sizeof bad);
    bad[2] = 2u;
    TEST_ASSERT_FALSE(zws_decode(bad, sizeof bad, keep));
    TEST_ASSERT_FALSE(zws_decode(ZWS_FIXTURE_V1, sizeof ZWS_FIXTURE_V1 - 1u, keep));
    TEST_ASSERT_FALSE(zws_decode(ZWS_FIXTURE_V1, 3u, keep));

    /* a short entry (an older layout) and a long one (a newer), CRC from the C side here: the
     * layout's reading, not the format's (that is the fixture's) */
    uint8_t mixed[64];
    size_t k = 0u;
    mixed[k++] = 'Z';
    mixed[k++] = 'S';
    mixed[k++] = 1u;
    mixed[k++] = 2u;
    const uint8_t shortie[] = {8u, 0u, 0x00, 0x07, 0x00, 0x01, 0x00, 0x01, 0x01};
    memcpy(&mixed[k], shortie, sizeof shortie);
    k += sizeof shortie;
    mixed[k++] = ZWS_ENTRY + 2u;
    const uint8_t longie[ZWS_ENTRY + 2u] = {2u, 0x00, 0x09, 0x00, 0x02, 0x00, 0x02, 0x01, 0x00,
                                             0x02, 0, 0, 0, 0, 0, 0, 0x00, 0x3C, 0xEE, 0xEE};
    memcpy(&mixed[k], longie, sizeof longie);
    k += sizeof longie;
    const uint16_t crc = siren_crc16(mixed, k);
    mixed[k++] = (uint8_t)(crc >> 8);
    mixed[k++] = (uint8_t)(crc & 0xFFu);
    TEST_ASSERT_TRUE(zws_decode(mixed, k, got));
    TEST_ASSERT_TRUE(got[0].used);
    TEST_ASSERT_EQUAL_UINT16(7u, got[0].node);
    TEST_ASSERT_EQUAL_HEX8(0x01u, got[0].partitions);
    TEST_ASSERT_FALSE(got[0].burglary); /* its flags were cut off: 0 */
    TEST_ASSERT_EQUAL_UINT16(0u, got[0].supervision_min);
    TEST_ASSERT_TRUE(got[2].used);
    TEST_ASSERT_EQUAL_UINT16(9u, got[2].node);
    TEST_ASSERT_EQUAL_UINT8(ZWS_KIND_BINARY, got[2].kind);
    TEST_ASSERT_EQUAL_UINT16(60u, got[2].supervision_min);
}

/* OUT-019: read back at start-up nothing is sent and its silence counts from the first poll. */
static void test_load_starts_quiet(void)
{
    start();
    zws_config_t got[ZWS_SIRENS];
    TEST_ASSERT_TRUE(zws_decode(ZWS_FIXTURE_V1, sizeof ZWS_FIXTURE_V1, got));
    zws_load(&Z, got);
    TEST_ASSERT_EQUAL_UINT64(5000u, zws_next_deadline(&Z, 5000u));
    zws_poll(&Z, 5000u);
    TEST_ASSERT_EQUAL_UINT(0u, F.n);
    TEST_ASSERT_EQUAL_UINT64(5000u + 120u * 60000u, zws_next_deadline(&Z, 5000u));
    TEST_ASSERT_FALSE(Z.dirty);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_follows_the_bell_with_a_tone_per_kind);
    RUN_TEST(test_default_tone_volume_and_endpoint);
    RUN_TEST(test_scope_and_also);
    RUN_TEST(test_binary_switch_fallback);
    RUN_TEST(test_confirm_retry_and_trouble);
    RUN_TEST(test_refused_and_unanswered_sends);
    RUN_TEST(test_keep_up_while_sounding);
    RUN_TEST(test_stop_given_up_until_heard);
    RUN_TEST(test_supervision_silence);
    RUN_TEST(test_supervision_limit_and_rules);
    RUN_TEST(test_squawk);
    RUN_TEST(test_test_and_refusals);
    RUN_TEST(test_store_and_fixture);
    RUN_TEST(test_load_starts_quiet);
    return UNITY_END();
}
