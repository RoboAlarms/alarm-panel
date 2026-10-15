/*
 * test_zw_s2.c - the S2 security manager (components/core/zwave_host/zw_s2.h, docs/features/36
 * security.md §6-9, command-classes.md §1): a panel (node 1) and a simulated device (node 2),
 * each with its own manager and crypto, over a pretend radio on a fake clock that can lose,
 * repeat, hold and cross frames. Also the Security 2 command class's wire format and §7's
 * extension rules, and §11's encapsulation vector decrypted by the manager.
 *
 * Requirements: ZW-007 (the S2 runtime: SPAN establishment and resync, 5-nonce tolerance,
 * crossed frames, duplicates, nonce reports without floods), ZW-009 (the security-level rule,
 * frames from unknown nodes dropped), ZW-037 (a frame the radio did not deliver sent again as a
 * new frame, 3 transmissions in all, never to a sleeping node, never a Nonce Report).
 */
#include <string.h>

#include "unity.h"
#include "zw_host_crypto.h"
#include "zwave_host/zw_s2.h"
#include "zwave_proto/zw_cc.h"
#include "zwave_proto/zw_cc_encap.h"

#define PANEL 1u
#define DEV   2u
#define HOME  0xC0FFEE01u
#define AIR   64u
/* security.md §6's times, as literals: a library constant that drifts fails the tests. */
#define RESEND_MS      500u
#define CROSSED_MS     500u
#define NONCE_WAIT_MS 1000u

static zw_host_crypto_t panel_hc, dev_hc;
static zw_crypto_port_t panel_crypto, dev_crypto;
static zw_s2_t panel, dev;
static zw_s2_keys_t keys;
static uint64_t now;

/* A secure sensor's node information: Z-Wave Plus Info, Transport Service, Security 2,
 * Supervision - everything else it supports only securely. */
static const uint8_t NIF[] = {0x5E, 0x55, 0x9F, 0x6C};
static const uint8_t BATTERY_LOW[] = {0x80, 0x03, 0xFF};
static const uint8_t BASIC_GET[] = {0x20, 0x02};
static const uint8_t PLUS_INFO_REPORT[] = {0x5E, 0x02, 0x02, 0x00, 0x00, 0x0C, 0x07, 0x0C, 0x07};
static uint8_t door_open[16], door_closed[16];
static size_t door_len;

/* ---- The radio ---------------------------------------------------------------------------- */

typedef struct {
    uint16_t from, to;
    uint8_t opt;
    uint32_t tag;
    size_t len;
    uint8_t b[ZW_S2_FRAME_MAX];
} air_t;

static air_t air[AIR];
static size_t air_head, air_tail;
static bool hold;              /* frames stay in the air until released */
static unsigned lose_dev;      /* the device's next frames are lost */
static unsigned lose_panel;    /* the panel's next frames are lost */
static unsigned repeat_dev;    /* the device's next frames arrive twice */
static unsigned crc_dev;       /* the device's next frames arrive inside CRC-16 */
static air_t heard[AIR];       /* what went over the air, in order */
static size_t heard_n;

static bool on_air(uint16_t from, uint16_t to, const uint8_t *f, size_t len, uint8_t opt,
                   uint32_t tag)
{
    TEST_ASSERT_TRUE_MESSAGE(air_tail - air_head < AIR, "the air is full");
    TEST_ASSERT_TRUE(len <= ZW_S2_FRAME_MAX);
    air_t *a = &air[air_tail++ % AIR];
    a->from = from;
    a->to = to;
    a->opt = opt;
    a->tag = tag;
    a->len = len;
    memcpy(a->b, f, len);
    return true;
}

static zw_s2_t *side(uint16_t node)
{
    return node == PANEL ? &panel : &dev;
}

static void pump(void)
{
    while (!hold && air_head != air_tail) {
        const air_t a = air[air_head++ % AIR];
        bool lost = false;
        if (a.from == DEV && lose_dev > 0u) {
            lose_dev--;
            lost = true;
        } else if (a.from == PANEL && lose_panel > 0u) {
            lose_panel--;
            lost = true;
        }
        if (!lost) {
            if (heard_n < AIR) {
                heard[heard_n++] = a;
            }
            if (a.from == DEV && crc_dev > 0u) {
                crc_dev--;
                uint8_t w[ZW_S2_FRAME_MAX + 4u];
                const size_t wn = zw_cc_wrap_crc16(w, sizeof w, a.b, a.len);
                TEST_ASSERT_TRUE(wn > 0u);
                zw_s2_receive(side(a.to), now, a.from, w, wn);
            } else {
                zw_s2_receive(side(a.to), now, a.from, a.b, a.len);
            }
            if (a.from == DEV && repeat_dev > 0u) {
                repeat_dev--;
                zw_s2_receive(side(a.to), now, a.from, a.b, a.len);
            }
        }
        zw_s2_sent(side(a.from), now, a.tag, !lost);
    }
}

static void run_to(uint64_t t)
{
    pump();
    for (;;) {
        const uint64_t a = zw_s2_next_deadline(&panel);
        const uint64_t b = zw_s2_next_deadline(&dev);
        const uint64_t d = a < b ? a : b;
        if (d > t) {
            break;
        }
        if (d > now) {
            now = d;
        }
        zw_s2_poll(&panel, now);
        zw_s2_poll(&dev, now);
        pump();
    }
    now = t;
    zw_s2_poll(&panel, now);
    zw_s2_poll(&dev, now);
    pump();
}

/* ---- What each side got ------------------------------------------------------------------- */

typedef struct {
    zw_s2_rx_t rx;
    uint8_t cmd[64];
} got_t;

static got_t got[2][16];
static size_t got_n[2];
static struct {
    uint32_t tag;
    zw_s2_result_t r;
    uint64_t at;
} done[2][16];
static size_t done_n[2];
static zw_s2_event_t events[2][32];
static size_t events_n[2];

static int who(void *ctx)
{
    return ctx == (void *)&panel ? 0 : 1;
}

static bool panel_send(void *ctx, uint16_t node, const uint8_t *f, size_t len, uint8_t opt,
                       uint32_t tag)
{
    (void)ctx;
    return on_air(PANEL, node, f, len, opt, tag);
}

static bool dev_send(void *ctx, uint16_t node, const uint8_t *f, size_t len, uint8_t opt,
                     uint32_t tag)
{
    (void)ctx;
    return on_air(DEV, node, f, len, opt, tag);
}

static void on_received(void *ctx, const zw_s2_rx_t *rx)
{
    const int w = who(ctx);
    TEST_ASSERT_TRUE(got_n[w] < 16u && rx->len <= 64u);
    got_t *g = &got[w][got_n[w]++];
    g->rx = *rx;
    memcpy(g->cmd, rx->command, rx->len);
    g->rx.command = g->cmd;
}

static void on_send_done(void *ctx, uint32_t tag, zw_s2_result_t r)
{
    const int w = who(ctx);
    TEST_ASSERT_TRUE(done_n[w] < 16u);
    done[w][done_n[w]].tag = tag;
    done[w][done_n[w]].r = r;
    done[w][done_n[w]].at = now;
    done_n[w]++;
}

static void on_event(void *ctx, uint16_t node, zw_s2_event_t ev)
{
    (void)node;
    const int w = who(ctx);
    if (events_n[w] < 32u) {
        events[w][events_n[w]++] = ev;
    }
}

static unsigned count_events(int w, zw_s2_event_t ev)
{
    unsigned n = 0;
    for (size_t i = 0; i < events_n[w]; i++) {
        n += events[w][i] == ev ? 1u : 0u;
    }
    return n;
}

/* ---- The owner's transmissions (serial-api.md §6.12) ----------------------------------------- */

static bool dev_sleeps;        /* the panel's owner takes the device for a sleeping one */
static unsigned asked;         /* times the panel's owner was asked */
static uint16_t asked_node;
static uint32_t asked_tag;

static uint8_t panel_attempts(void *ctx, uint16_t node, uint32_t tag)
{
    TEST_ASSERT_TRUE(ctx == (void *)&panel);
    asked++;
    asked_node = node;
    asked_tag = tag;
    return dev_sleeps ? 1u : (uint8_t)ZW_S2_TX_ATTEMPTS;
}

static unsigned jam_asked;     /* times the panel's owner was asked about a jammed frame */
static uint8_t jam_last;       /* ...and the jam count it was given last */

/* The owner of a jammed module: always the same frame again after the wait (§6.12). */
static zw_s2_jam_t panel_jammed(void *ctx, uint16_t node, uint32_t tag, uint8_t jams)
{
    TEST_ASSERT_TRUE(ctx == (void *)&panel);
    (void)node;
    (void)tag;
    jam_asked++;
    jam_last = jams;
    return ZW_S2_JAM_WAIT;
}

/* Frames the panel put on the air from index `from` of the ring on (lost ones too). */
static size_t panel_frames_since(size_t from)
{
    size_t n = 0;
    for (size_t i = from; i < air_tail; i++) {
        n += air[i % AIR].from == PANEL ? 1u : 0u;
    }
    return n;
}

/* ---- Set-up ------------------------------------------------------------------------------- */

static void panel_up(void)
{
    TEST_ASSERT_TRUE(zw_s2_start(&panel, HOME, PANEL, &keys));
}

static void dev_up(uint8_t granted)
{
    TEST_ASSERT_TRUE(zw_s2_start(&dev, HOME, DEV, NULL));
    if ((granted & ZW_S2_KEY_UNAUTH) != 0u) {
        TEST_ASSERT_TRUE(zw_s2_set_key(&dev, ZW_S2_KEY_UNAUTH, keys.s2_unauth));
    }
    if ((granted & ZW_S2_KEY_AUTH) != 0u) {
        TEST_ASSERT_TRUE(zw_s2_set_key(&dev, ZW_S2_KEY_AUTH, keys.s2_auth));
    }
    if ((granted & ZW_S2_KEY_ACCESS) != 0u) {
        TEST_ASSERT_TRUE(zw_s2_set_key(&dev, ZW_S2_KEY_ACCESS, keys.s2_access));
    }
    TEST_ASSERT_TRUE(zw_s2_add_node(&dev, PANEL, granted, NULL, 0u));
}

/* The panel and a device holding `granted`, no SPAN between them yet. */
static void pair(uint8_t granted)
{
    panel_up();
    TEST_ASSERT_TRUE(zw_s2_add_node(&panel, DEV, granted, NIF, sizeof NIF));
    dev_up(granted);
}

void setUp(void)
{
    static const zw_s2_port_t pp = {&panel, panel_send, on_received, on_send_done, on_event};
    static const zw_s2_port_t dp = {&dev, dev_send, on_received, on_send_done, on_event};
    const zw_s2_config_t pcfg = {true, 0u};
    const zw_s2_config_t dcfg = {false, 0u};
    zw_host_crypto_init(&panel_hc, 0x5041u);
    zw_host_crypto_init(&dev_hc, 0x4445u);
    panel_crypto = zw_host_crypto_port(&panel_hc);
    dev_crypto = zw_host_crypto_port(&dev_hc);
    zw_s2_init(&panel, &panel_crypto, &pp, &pcfg);
    zw_s2_init(&dev, &dev_crypto, &dp, &dcfg);
    memset(keys.s2_unauth, 0x11, 16);
    memset(keys.s2_auth, 0x22, 16);
    memset(keys.s2_access, 0x33, 16);
    memset(keys.s0, 0x44, 16);
    memset(keys.lr_auth, 0x55, 16);
    memset(keys.lr_access, 0x66, 16);
    now = 1000u;
    air_head = air_tail = heard_n = 0u;
    hold = false;
    lose_dev = lose_panel = repeat_dev = 0u;
    dev_sleeps = false;
    asked = 0u;
    asked_node = 0u;
    asked_tag = 0u;
    jam_asked = 0u;
    jam_last = 0u;
    memset(got_n, 0, sizeof got_n);
    memset(done_n, 0, sizeof done_n);
    memset(events_n, 0, sizeof events_n);
    /* Notification Reports: Access Control, door open (16) and closed (17) */
    zw_cc_notification_report_t r;
    memset(&r, 0, sizeof r);
    r.status = ZW_NOTIFICATION_STATUS_ON;
    r.notification_type = ZW_NOTIFICATION_TYPE_ACCESS_CONTROL;
    r.event = 0x16u;
    door_len = zw_cc_notification_report_build(door_open, sizeof door_open, &r);
    r.event = 0x17u;
    TEST_ASSERT_EQUAL_UINT(door_len, zw_cc_notification_report_build(door_closed,
                                                                     sizeof door_closed, &r));
}

void tearDown(void)
{
}

/* The device sends `cmd` to the panel at `cls`, and the air settles. */
static void from_dev(zw_sec_class_t cls, const uint8_t *cmd, size_t len)
{
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_s2_send(&dev, now, PANEL, cls, cmd, len, 0x100u));
    pump();
}

static void from_panel(zw_sec_class_t cls, const uint8_t *cmd, size_t len, uint32_t tag)
{
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_s2_send(&panel, now, DEV, cls, cmd, len, tag));
    pump();
}

static void expect_got_at(int w, size_t i, zw_sec_class_t sec, const uint8_t *cmd, size_t len,
                          unsigned line)
{
    UNITY_TEST_ASSERT(got_n[w] > i, line, "not received");
    UNITY_TEST_ASSERT_EQUAL_INT(sec, got[w][i].rx.security, line, "class");
    UNITY_TEST_ASSERT_EQUAL_UINT(len, got[w][i].rx.len, line, "length");
    UNITY_TEST_ASSERT_EQUAL_HEX8_ARRAY(cmd, got[w][i].cmd, len, line, "command");
}
#define expect_got(w, i, sec, cmd, len) expect_got_at(w, i, sec, cmd, len, __LINE__)

/* ---- The wire format (§5-7, §9) ------------------------------------------------------------ */

/* ZW-007: every Security 2 command builds and parses back; a wrong command byte or a short
 * command is refused. */
static void test_codecs_round_trip(void)
{
    uint8_t b[64];
    const zw_s2_kex_t kex = {0x02u, ZW_S2_KEX_SCHEME_1, ZW_S2_KEX_CURVE_25519, 0x87u};
    zw_s2_kex_t k2;
    TEST_ASSERT_EQUAL_UINT(6, zw_s2_kex_report_build(b, sizeof b, &kex));
    static const uint8_t REPORT[] = {0x9F, 0x05, 0x02, 0x02, 0x01, 0x87};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(REPORT, b, 6);
    TEST_ASSERT_TRUE(zw_s2_kex_report_parse(b, 6, &k2));
    TEST_ASSERT_EQUAL_HEX8(0x87, k2.keys);
    TEST_ASSERT_FALSE(zw_s2_kex_set_parse(b, 6, &k2)); /* a Report is not a Set */
    TEST_ASSERT_FALSE(zw_s2_kex_report_parse(b, 5, &k2));
    TEST_ASSERT_EQUAL_UINT(6, zw_s2_kex_set_build(b, sizeof b, &kex));
    TEST_ASSERT_EQUAL_HEX8(0x06, b[1]);
    TEST_ASSERT_TRUE(zw_s2_kex_set_parse(b, 6, &k2));
    TEST_ASSERT_EQUAL_UINT(2, zw_s2_kex_get_build(b, sizeof b));
    TEST_ASSERT_TRUE(zw_s2_kex_get_parse(b, 2));
    uint8_t v = 0;
    TEST_ASSERT_EQUAL_UINT(3, zw_s2_kex_fail_build(b, sizeof b, ZW_S2_KEX_FAIL_CANCEL));
    TEST_ASSERT_TRUE(zw_s2_kex_fail_parse(b, 3, &v));
    TEST_ASSERT_EQUAL_HEX8(0x06, v);
    uint8_t key[32];
    for (unsigned i = 0; i < 32; i++) {
        key[i] = (uint8_t)i;
    }
    const zw_s2_pubkey_t pk = {true, key};
    zw_s2_pubkey_t pk2;
    TEST_ASSERT_EQUAL_UINT(35, zw_s2_pubkey_build(b, sizeof b, &pk));
    TEST_ASSERT_EQUAL_HEX8(0x01, b[2]);
    TEST_ASSERT_TRUE(zw_s2_pubkey_parse(b, 35, &pk2));
    TEST_ASSERT_TRUE(pk2.including);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(key, pk2.key, 32);
    TEST_ASSERT_FALSE(zw_s2_pubkey_parse(b, 34, &pk2));
    TEST_ASSERT_EQUAL_UINT(3, zw_s2_netkey_get_build(b, sizeof b, ZW_S2_KEY_AUTH));
    TEST_ASSERT_TRUE(zw_s2_netkey_get_parse(b, 3, &v));
    TEST_ASSERT_EQUAL_HEX8(0x02, v);
    const zw_s2_netkey_report_t nk = {ZW_S2_KEY_ACCESS, key};
    zw_s2_netkey_report_t nk2;
    TEST_ASSERT_EQUAL_UINT(19, zw_s2_netkey_report_build(b, sizeof b, &nk));
    TEST_ASSERT_TRUE(zw_s2_netkey_report_parse(b, 19, &nk2));
    TEST_ASSERT_EQUAL_HEX8(0x04, nk2.key);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(key, nk2.netkey, 16);
    TEST_ASSERT_EQUAL_UINT(2, zw_s2_netkey_verify_build(b, sizeof b));
    TEST_ASSERT_TRUE(zw_s2_netkey_verify_parse(b, 2));
    TEST_ASSERT_EQUAL_UINT(3, zw_s2_transfer_end_build(b, sizeof b, ZW_S2_TE_VERIFIED));
    TEST_ASSERT_TRUE(zw_s2_transfer_end_parse(b, 3, &v));
    TEST_ASSERT_EQUAL_HEX8(0x02, v);
    TEST_ASSERT_EQUAL_UINT(3, zw_s2_nonce_get_build(b, sizeof b, 0x42));
    TEST_ASSERT_TRUE(zw_s2_nonce_get_parse(b, 3, &v));
    TEST_ASSERT_EQUAL_HEX8(0x42, v);
    const zw_s2_nonce_report_t nr = {0x43, ZW_S2_NONCE_SOS, key};
    zw_s2_nonce_report_t nr2;
    TEST_ASSERT_EQUAL_UINT(20, zw_s2_nonce_report_build(b, sizeof b, &nr));
    TEST_ASSERT_TRUE(zw_s2_nonce_report_parse(b, 20, &nr2));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(key, nr2.receiver_ei, 16);
    TEST_ASSERT_FALSE(zw_s2_nonce_report_parse(b, 19, &nr2)); /* SOS without its 16 bytes */
    const zw_s2_nonce_report_t mos = {0x44, ZW_S2_NONCE_MOS, NULL};
    TEST_ASSERT_EQUAL_UINT(4, zw_s2_nonce_report_build(b, sizeof b, &mos));
    TEST_ASSERT_TRUE(zw_s2_nonce_report_parse(b, 4, &nr2));
    TEST_ASSERT_NULL(nr2.receiver_ei);
    const uint8_t *ccs = NULL;
    size_t n = 99;
    TEST_ASSERT_EQUAL_UINT(2, zw_s2_commands_supported_report_build(b, sizeof b, NULL, 0));
    TEST_ASSERT_TRUE(zw_s2_commands_supported_report_parse(b, 2, &ccs, &n));
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_UINT(0, zw_s2_kex_report_build(b, 5, &kex)); /* does not fit */
}

/* ZW-007, security.md §7: the extension rules - a known extension in the wrong place or with the
 * wrong length is skipped, an unknown one skipped unless critical, a length below 2 or past the
 * frame drops it, and "more" chains them. */
static void test_extension_rules(void)
{
    zw_s2_ext_t x;
    size_t used = 0;
    uint8_t span[18] = {18, 0x41};
    memset(&span[2], 0xA5, 16);
    TEST_ASSERT_TRUE(zw_s2_ext_parse(span, sizeof span, false, &x, &used));
    TEST_ASSERT_TRUE(x.span);
    TEST_ASSERT_EQUAL_UINT(18, used);
    TEST_ASSERT_TRUE(zw_s2_ext_parse(span, sizeof span, true, &x, &used)); /* encrypted SPAN */
    TEST_ASSERT_FALSE(x.span);
    static const uint8_t MOS_MGRP[] = {2, 0x84, 3, 0x43, 0x07}; /* MOS, more; MGRP group 7 */
    TEST_ASSERT_TRUE(zw_s2_ext_parse(MOS_MGRP, sizeof MOS_MGRP, false, &x, &used));
    TEST_ASSERT_TRUE(x.mos && x.mgrp);
    TEST_ASSERT_EQUAL_HEX8(0x07, x.group);
    TEST_ASSERT_EQUAL_UINT(5, used);
    uint8_t mpan[19] = {19, 0x42};
    TEST_ASSERT_TRUE(zw_s2_ext_parse(mpan, sizeof mpan, false, &x, &used)); /* unencrypted MPAN */
    TEST_ASSERT_FALSE(x.mpan);
    TEST_ASSERT_TRUE(zw_s2_ext_parse(mpan, sizeof mpan, true, &x, &used));
    TEST_ASSERT_TRUE(x.mpan);
    static const uint8_t UNKNOWN[] = {3, 0x15, 0x00};          /* type 15, not critical */
    TEST_ASSERT_TRUE(zw_s2_ext_parse(UNKNOWN, sizeof UNKNOWN, false, &x, &used));
    static const uint8_t UNKNOWN_CRIT[] = {3, 0x55, 0x00};     /* type 15, critical */
    TEST_ASSERT_FALSE(zw_s2_ext_parse(UNKNOWN_CRIT, sizeof UNKNOWN_CRIT, false, &x, &used));
    static const uint8_t SHORT_SPAN[] = {3, 0x41, 0x00};       /* SPAN of 3 bytes: skipped */
    TEST_ASSERT_TRUE(zw_s2_ext_parse(SHORT_SPAN, sizeof SHORT_SPAN, false, &x, &used));
    TEST_ASSERT_FALSE(x.span);
    static const uint8_t LEN1[] = {1, 0x04};                   /* a length below 2 */
    TEST_ASSERT_FALSE(zw_s2_ext_parse(LEN1, sizeof LEN1, false, &x, &used));
    static const uint8_t PAST[] = {5, 0x04, 0x00};             /* past the frame */
    TEST_ASSERT_FALSE(zw_s2_ext_parse(PAST, sizeof PAST, false, &x, &used));
    static const uint8_t MORE_NONE[] = {2, 0x84};              /* "more", and nothing */
    TEST_ASSERT_FALSE(zw_s2_ext_parse(MORE_NONE, sizeof MORE_NONE, false, &x, &used));
    /* and in a frame: an unknown critical extension drops the whole Message Encapsulation */
    uint8_t f[4 + 3 + 8] = {0x9F, 0x03, 0x10, 0x01, 3, 0x55, 0x00};
    zw_s2_encap_t e;
    TEST_ASSERT_FALSE(zw_s2_encap_parse(f, sizeof f, &e));
    f[5] = 0x15; /* not critical: parses, nothing before the tag */
    TEST_ASSERT_TRUE(zw_s2_encap_parse(f, sizeof f, &e));
    TEST_ASSERT_EQUAL_UINT(3, e.ext_len);
    TEST_ASSERT_EQUAL_UINT(0, e.ciphertext_len);
    /* an extension may not run into the tag */
    uint8_t g[4 + 2 + 8] = {0x9F, 0x03, 0x10, 0x01, 9, 0x04};
    TEST_ASSERT_FALSE(zw_s2_encap_parse(g, sizeof g, &e));
}

/* ZW-SEC-010 through the manager: §11's frame from node 2, on the panel's receiver EI, decrypts
 * to the door-open report at the class of the key, and the next frame to Battery low. */
static void test_vector_frames_decrypt(void)
{
    static const uint8_t PNK[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    static const uint8_t REI[16] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                                    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f};
    static const uint8_t F1[] = {0x9f, 0x03, 0x55, 0x01, 0x12, 0x41, 0x10, 0x11, 0x12, 0x13,
                                 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d,
                                 0x1e, 0x1f, 0xee, 0x50, 0xc2, 0xd6, 0x01, 0xc4, 0x1a, 0xd2,
                                 0x85, 0x02, 0xad, 0xd7, 0x58, 0x92, 0x9a, 0x40, 0xad};
    static const uint8_t F2[] = {0x9f, 0x03, 0x56, 0x00, 0xc7, 0xd3, 0x8e, 0xf4,
                                 0x7c, 0x7b, 0x14, 0x63, 0x53, 0x2a, 0x4b};
    static const uint8_t INNER1[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
    memcpy(keys.s2_unauth, PNK, 16);
    panel_up();
    TEST_ASSERT_TRUE(zw_s2_add_node(&panel, DEV, ZW_S2_KEY_UNAUTH, NIF, sizeof NIF));
    zw_s2_peer_t *p = &panel.peers[0];
    TEST_ASSERT_EQUAL_UINT16(DEV, p->node);
    p->span_state = ZW_S2_SPAN_LOCAL_EI; /* as after its Nonce Report with this receiver EI */
    p->local_ei_valid = true;
    memcpy(p->local_ei, REI, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(INNER1, door_open, sizeof INNER1); /* the codec agrees */
    zw_s2_receive(&panel, now, DEV, F1, sizeof F1);
    expect_got(0, 0, ZW_SEC_S2_UNAUTH, INNER1, sizeof INNER1);
    TEST_ASSERT_EQUAL_INT(ZW_S2_SPAN_READY, zw_s2_span_state(&panel, DEV));
    zw_s2_receive(&panel, now, DEV, F2, sizeof F2);
    expect_got(0, 1, ZW_SEC_S2_UNAUTH, BATTERY_LOW, sizeof BATTERY_LOW);
    TEST_ASSERT_EQUAL_UINT(0, air_tail); /* no Nonce Report was needed */
    /* the same frame again is a duplicate */
    zw_s2_receive(&panel, now, DEV, F2, sizeof F2);
    TEST_ASSERT_EQUAL_UINT(2, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(1, count_events(0, ZW_S2_EV_DUPLICATE));
}

/* §11's SPAN on the panel's side for node 2 (key PNK as S2 Unauthenticated), frame 1 received:
 * the generator stands after nonce 1. */
static void vector_span(void)
{
    static const uint8_t PNK[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    static const uint8_t REI[16] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                                    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f};
    static const uint8_t F1[] = {0x9f, 0x03, 0x55, 0x01, 0x12, 0x41, 0x10, 0x11, 0x12, 0x13,
                                 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d,
                                 0x1e, 0x1f, 0xee, 0x50, 0xc2, 0xd6, 0x01, 0xc4, 0x1a, 0xd2,
                                 0x85, 0x02, 0xad, 0xd7, 0x58, 0x92, 0x9a, 0x40, 0xad};
    memcpy(keys.s2_unauth, PNK, 16);
    panel_up();
    TEST_ASSERT_TRUE(zw_s2_add_node(&panel, DEV, ZW_S2_KEY_UNAUTH, NIF, sizeof NIF));
    panel.peers[0].span_state = ZW_S2_SPAN_LOCAL_EI;
    panel.peers[0].local_ei_valid = true;
    memcpy(panel.peers[0].local_ei, REI, 16);
    zw_s2_receive(&panel, now, DEV, F1, sizeof F1);
    TEST_ASSERT_EQUAL_UINT(1, got_n[0]);
}

/* A frame from node 2 on §11's key, seq 0x56, encrypted with `nonce`; an MGRP extension with
 * `mgrp`. */
static size_t vector_frame(uint8_t *f, bool mgrp, const uint8_t nonce[13])
{
    static const uint8_t KEY_CCM[16] = {0x27, 0xf9, 0x0d, 0xd6, 0xd7, 0x46, 0x97, 0xcb,
                                        0xa5, 0x4e, 0xb1, 0x1a, 0x0e, 0xe8, 0x06, 0x17};
    size_t h = 0;
    f[h++] = 0x9F;
    f[h++] = 0x03;
    f[h++] = 0x56;
    f[h++] = mgrp ? 0x01 : 0x00;
    if (mgrp) {
        f[h++] = 3;
        f[h++] = 0x43; /* critical, MGRP */
        f[h++] = 0x07;
    }
    const size_t total = h + sizeof BATTERY_LOW + 8u;
    uint8_t aad[48];
    const size_t a = zw_s2_aad_build(aad, sizeof aad, DEV, PANEL, HOME, (uint16_t)total, &f[2],
                                     h - 2u);
    TEST_ASSERT_TRUE(a > 0u);
    TEST_ASSERT_TRUE(zw_aes_ccm_encrypt(&panel_crypto, KEY_CCM, nonce, aad, a, BATTERY_LOW,
                                        sizeof BATTERY_LOW, &f[h], &f[h + sizeof BATTERY_LOW]));
    return total;
}

/* ZW-007, §6: a frame without a SPAN extension gets up to 5 nonces - but a multicast follow-up
 * (MGRP extension) only one: with §11's nonce 3 (one frame skipped), the plain frame decrypts,
 * the follow-up does not. */
static void test_multicast_follow_up_one_attempt(void)
{
    static const uint8_t NONCE3[13] = {0xed, 0xd7, 0x5c, 0x79, 0x13, 0x8b, 0x6b,
                                       0xf0, 0x58, 0x47, 0x4a, 0x91, 0x93};
    uint8_t f[40];
    vector_span();
    size_t n = vector_frame(f, false, NONCE3);
    zw_s2_receive(&panel, now, DEV, f, n);
    expect_got(0, 1, ZW_SEC_S2_UNAUTH, BATTERY_LOW, sizeof BATTERY_LOW);
    setUp();
    vector_span();
    n = vector_frame(f, true, NONCE3);
    zw_s2_receive(&panel, now, DEV, f, n);
    TEST_ASSERT_EQUAL_UINT(1, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(1, count_events(0, ZW_S2_EV_DECRYPT_FAILED));
}

/* ---- SPANs (§6) --------------------------------------------------------------------------- */

/* ZW-007: first contact - the device's report needs a nonce: Nonce Get, the panel's Nonce Report
 * (transmit options 05), then the report with a SPAN extension, decrypted at the device's highest
 * class; the device's send ends 500 ms after delivery, when no Nonce Report came back. */
static void test_first_frame_establishes_span(void)
{
    pair(ZW_S2_KEY_AUTH);
    from_dev(ZW_SEC_AUTO, door_open, door_len);
    TEST_ASSERT_EQUAL_UINT(3, heard_n);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_NONCE_GET, heard[0].b[1]);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_NONCE_REPORT, heard[1].b[1]);
    TEST_ASSERT_EQUAL_HEX8(0x05, heard[1].opt);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_MESSAGE_ENCAPSULATION, heard[2].b[1]);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_ENCAP_EXT, heard[2].b[3]); /* a SPAN extension */
    TEST_ASSERT_EQUAL_HEX8(0x25, heard[2].opt);
    expect_got(0, 0, ZW_SEC_S2_AUTH, door_open, door_len);
    TEST_ASSERT_EQUAL_INT(ZW_S2_SPAN_READY, zw_s2_span_state(&panel, DEV));
    TEST_ASSERT_EQUAL_INT(ZW_S2_SPAN_READY, zw_s2_span_state(&dev, PANEL));
    TEST_ASSERT_EQUAL_UINT(0, done_n[1]);
    run_to(now + RESEND_MS - 1u);
    TEST_ASSERT_EQUAL_UINT(0, done_n[1]);
    run_to(now + 1u);
    TEST_ASSERT_EQUAL_UINT(1, done_n[1]);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, done[1][0].r);
    /* the next report needs no nonce */
    from_dev(ZW_SEC_AUTO, BATTERY_LOW, sizeof BATTERY_LOW);
    TEST_ASSERT_EQUAL_UINT(4, heard_n);
    TEST_ASSERT_EQUAL_HEX8(0x00, heard[3].b[3]); /* no extension */
    expect_got(0, 1, ZW_SEC_S2_AUTH, BATTERY_LOW, sizeof BATTERY_LOW);
    /* and the panel's commands go on the same SPAN */
    from_panel(ZW_SEC_AUTO, BASIC_GET, sizeof BASIC_GET, 7u);
    expect_got(1, 0, ZW_SEC_S2_AUTH, BASIC_GET, sizeof BASIC_GET);
    TEST_ASSERT_EQUAL_UINT(1, zw_s2_node_stats(&panel, DEV)->nonce_reports);
}

/* ZW-007: a command that fits a frame but not with a new SPAN's extension: the SPAN goes ahead in
 * a frame of its own (nothing to hand over), then the command on the next nonce. */
static void test_span_frame_ahead_of_long_command(void)
{
    pair(ZW_S2_KEY_AUTH);
    uint8_t in[24], f[32];
    size_t n = zw_cc_wrap_multi_channel(in, sizeof in, 1u, 0u, false, door_open, door_len);
    n = zw_cc_wrap_supervision(f, sizeof f, 7u, false, in, n);
    TEST_ASSERT_EQUAL_UINT(17, n); /* 4 + 18 + 17 + 8 = 47 > 46 */
    from_dev(ZW_SEC_S2_AUTH, f, n);
    TEST_ASSERT_EQUAL_UINT(4, heard_n);
    TEST_ASSERT_EQUAL_UINT(30, heard[2].len); /* header, SPAN, tag */
    TEST_ASSERT_EQUAL_UINT(29, heard[3].len); /* header, the command, tag */
    TEST_ASSERT_EQUAL_UINT(1, got_n[0]);
    expect_got(0, 0, ZW_SEC_S2_AUTH, door_open, door_len);
    TEST_ASSERT_TRUE(got[0][0].rx.supervised && got[0][0].rx.multi_channel);
    run_to(now + 1000u);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, done[1][0].r);
}

/* ZW-007: the panel restarts - its SPAN table is RAM only. The sensor's next report cannot be
 * decrypted: the panel answers a Nonce Report, the sensor sends it again on a new SPAN, and it
 * is delivered once. */
static void test_panel_restart_resync(void)
{
    pair(ZW_S2_KEY_AUTH);
    from_dev(ZW_SEC_AUTO, door_open, door_len);
    run_to(now + 1000u);
    panel_up();
    TEST_ASSERT_TRUE(zw_s2_add_node(&panel, DEV, ZW_S2_KEY_AUTH, NIF, sizeof NIF));
    TEST_ASSERT_EQUAL_INT(ZW_S2_SPAN_NONE, zw_s2_span_state(&panel, DEV));
    from_dev(ZW_SEC_AUTO, door_closed, door_len);
    TEST_ASSERT_EQUAL_UINT(2, got_n[0]);
    expect_got(0, 1, ZW_SEC_S2_AUTH, door_closed, door_len);
    TEST_ASSERT_EQUAL_UINT(1, count_events(0, ZW_S2_EV_DECRYPT_FAILED));
    TEST_ASSERT_EQUAL_UINT(1, zw_s2_node_stats(&panel, DEV)->decrypt_failures);
    TEST_ASSERT_EQUAL_UINT(1, zw_s2_node_stats(&dev, PANEL)->resends);
    run_to(now + 1000u);
    TEST_ASSERT_EQUAL_UINT(2, done_n[1]);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, done[1][1].r);
}

/* ZW-007: lost frames advance the sender's generator; the panel tries 5 nonces - 4 lost frames
 * are bridged; after 5 the next frame fails, a Nonce Report follows and it comes again. */
static void test_lost_frames(void)
{
    pair(ZW_S2_KEY_AUTH);
    from_dev(ZW_SEC_AUTO, door_open, door_len);
    run_to(now + 1000u);
    lose_dev = 4u;
    for (unsigned i = 0; i < 4; i++) {
        from_dev(ZW_SEC_AUTO, BATTERY_LOW, sizeof BATTERY_LOW);
    }
    TEST_ASSERT_EQUAL_UINT(1, got_n[0]);
    from_dev(ZW_SEC_AUTO, door_closed, door_len);
    expect_got(0, 1, ZW_SEC_S2_AUTH, door_closed, door_len);
    TEST_ASSERT_EQUAL_UINT(0, zw_s2_node_stats(&panel, DEV)->decrypt_failures);
    run_to(now + 1000u);
    lose_dev = 5u;
    for (unsigned i = 0; i < 5; i++) {
        from_dev(ZW_SEC_AUTO, BATTERY_LOW, sizeof BATTERY_LOW);
    }
    from_dev(ZW_SEC_AUTO, door_open, door_len);
    expect_got(0, 2, ZW_SEC_S2_AUTH, door_open, door_len);
    TEST_ASSERT_EQUAL_UINT(3, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(1, zw_s2_node_stats(&panel, DEV)->decrypt_failures);
    TEST_ASSERT_EQUAL_UINT(1, zw_s2_node_stats(&dev, PANEL)->resends);
}

/* ZW-007: crossed frames - both sides send at once on the same SPAN position; each takes the
 * other's frame with its own last sending nonce, and the SPAN stays in step. */
static void test_crossed_frames(void)
{
    pair(ZW_S2_KEY_AUTH);
    from_dev(ZW_SEC_AUTO, door_open, door_len);
    run_to(now + 1000u);
    hold = true;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_s2_send(&panel, now, DEV, ZW_SEC_AUTO, BASIC_GET,
                                               sizeof BASIC_GET, 1u));
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_s2_send(&dev, now, PANEL, ZW_SEC_AUTO, BATTERY_LOW,
                                               sizeof BATTERY_LOW, 2u));
    hold = false;
    pump();
    expect_got(1, 0, ZW_SEC_S2_AUTH, BASIC_GET, sizeof BASIC_GET);
    expect_got(0, 1, ZW_SEC_S2_AUTH, BATTERY_LOW, sizeof BATTERY_LOW);
    TEST_ASSERT_EQUAL_UINT(1, zw_s2_node_stats(&panel, DEV)->nonce_reports); /* no resync */
    TEST_ASSERT_EQUAL_UINT(0, zw_s2_node_stats(&dev, PANEL)->nonce_reports);
    run_to(now + 1000u);
    from_dev(ZW_SEC_AUTO, door_closed, door_len);
    expect_got(0, 2, ZW_SEC_S2_AUTH, door_closed, door_len);
    from_panel(ZW_SEC_AUTO, BASIC_GET, sizeof BASIC_GET, 3u);
    expect_got(1, 1, ZW_SEC_S2_AUTH, BASIC_GET, sizeof BASIC_GET);
    TEST_ASSERT_EQUAL_UINT(0, zw_s2_node_stats(&panel, DEV)->decrypt_failures);
    TEST_ASSERT_EQUAL_UINT(0, zw_s2_node_stats(&dev, PANEL)->decrypt_failures);
    /* the crossing nonce is good for 500 ms only: frames that crossed longer ago fail on both
     * sides, and a resync brings each one through */
    run_to(now + 1000u);
    hold = true;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_s2_send(&panel, now, DEV, ZW_SEC_AUTO, BASIC_GET,
                                               sizeof BASIC_GET, 4u));
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_s2_send(&dev, now, PANEL, ZW_SEC_AUTO, BATTERY_LOW,
                                               sizeof BATTERY_LOW, 5u));
    now += CROSSED_MS;
    hold = false;
    run_to(now + 1000u);
    expect_got(1, 2, ZW_SEC_S2_AUTH, BASIC_GET, sizeof BASIC_GET);
    expect_got(0, 3, ZW_SEC_S2_AUTH, BATTERY_LOW, sizeof BATTERY_LOW);
    TEST_ASSERT_EQUAL_UINT(4, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(3, got_n[1]);
    TEST_ASSERT_EQUAL_UINT(1, zw_s2_node_stats(&panel, DEV)->decrypt_failures);
    TEST_ASSERT_EQUAL_UINT(1, zw_s2_node_stats(&dev, PANEL)->decrypt_failures);
    /* and only for the peer's next sequence number: a crossing frame whose number skipped one
     * is not taken on the crossing nonce (a resync brings it) */
    run_to(now + 1000u);
    /* each side kept the SPAN of the other's resent frame: the next report resyncs them once */
    from_dev(ZW_SEC_AUTO, door_closed, door_len);
    TEST_ASSERT_EQUAL_UINT(5, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(2, zw_s2_node_stats(&panel, DEV)->decrypt_failures);
    run_to(now + 1000u);
    hold = true;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_s2_send(&panel, now, DEV, ZW_SEC_AUTO, BASIC_GET,
                                               sizeof BASIC_GET, 6u));
    dev.peers[0].own_seq = (uint8_t)(dev.peers[0].own_seq + 1u);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_s2_send(&dev, now, PANEL, ZW_SEC_AUTO, BATTERY_LOW,
                                               sizeof BATTERY_LOW, 7u));
    hold = false;
    run_to(now + 1000u);
    TEST_ASSERT_EQUAL_UINT(6, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(4, got_n[1]);
    TEST_ASSERT_EQUAL_UINT(3, zw_s2_node_stats(&panel, DEV)->decrypt_failures);
    TEST_ASSERT_EQUAL_UINT(1, zw_s2_node_stats(&dev, PANEL)->decrypt_failures);
}

/* ZW-007, §6 "after sending": the node's next frame, on the SPAN's next nonce and with its next
 * sequence number, shows it decoded the panel's frame - the panel's send ends then, not RESEND_MS
 * after delivery, and the next one to the node goes at once (a contact holds its next report until
 * it hears the Supervision Report to the last). A frame that crossed the panel's (on its own last
 * sending nonce) shows nothing: that send still ends RESEND_MS after delivery. */
static void test_nodes_next_frame_ends_the_wait(void)
{
    pair(ZW_S2_KEY_AUTH);
    from_dev(ZW_SEC_AUTO, door_open, door_len);
    run_to(now + 1000u);
    const size_t ended = done_n[0];
    from_panel(ZW_SEC_AUTO, BASIC_GET, sizeof BASIC_GET, 1u);
    from_panel(ZW_SEC_AUTO, BASIC_GET, sizeof BASIC_GET, 2u);
    TEST_ASSERT_EQUAL_UINT(1, got_n[1]); /* the second waits behind the first's watch */
    TEST_ASSERT_EQUAL_UINT(ended, done_n[0]);
    from_dev(ZW_SEC_AUTO, BATTERY_LOW, sizeof BATTERY_LOW); /* at the same moment */
    expect_got(0, 1, ZW_SEC_S2_AUTH, BATTERY_LOW, sizeof BATTERY_LOW);
    TEST_ASSERT_EQUAL_UINT(ended + 1u, done_n[0]);
    TEST_ASSERT_EQUAL_UINT32(1u, done[0][ended].tag);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, done[0][ended].r);
    expect_got(1, 1, ZW_SEC_S2_AUTH, BASIC_GET, sizeof BASIC_GET); /* the second went at once */
    run_to(now + 1000u);
    TEST_ASSERT_EQUAL_UINT(ended + 2u, done_n[0]);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, done[0][ended + 1u].r);
    /* crossed: the node's frame took the panel's frame's nonce - it may never have had the
     * panel's, so the panel's send waits out its watch */
    hold = true;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_s2_send(&panel, now, DEV, ZW_SEC_AUTO, BASIC_GET,
                                               sizeof BASIC_GET, 3u));
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_s2_send(&dev, now, PANEL, ZW_SEC_AUTO, BATTERY_LOW,
                                               sizeof BATTERY_LOW, 4u));
    hold = false;
    pump();
    expect_got(0, 2, ZW_SEC_S2_AUTH, BATTERY_LOW, sizeof BATTERY_LOW);
    TEST_ASSERT_EQUAL_UINT(ended + 2u, done_n[0]);
    run_to(now + RESEND_MS - 1u);
    TEST_ASSERT_EQUAL_UINT(ended + 2u, done_n[0]);
    run_to(now + 1u);
    TEST_ASSERT_EQUAL_UINT(ended + 3u, done_n[0]);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, done[0][ended + 2u].r);
    TEST_ASSERT_EQUAL_UINT(0, zw_s2_node_stats(&panel, DEV)->decrypt_failures);
    TEST_ASSERT_EQUAL_UINT(0, zw_s2_node_stats(&dev, PANEL)->decrypt_failures);
}

/* ZW-007: a frame repeated with the same sequence number (the device missed the ACK) is dropped
 * silently. */
static void test_duplicates_dropped(void)
{
    pair(ZW_S2_KEY_AUTH);
    repeat_dev = 3u; /* the Nonce Get, then the report */
    from_dev(ZW_SEC_AUTO, door_open, door_len);
    TEST_ASSERT_EQUAL_UINT(1, got_n[0]);
    TEST_ASSERT_TRUE(count_events(0, ZW_S2_EV_DUPLICATE) >= 1u);
    TEST_ASSERT_EQUAL_UINT(1, zw_s2_node_stats(&panel, DEV)->nonce_reports);
    run_to(now + 1000u);
    repeat_dev = 1u;
    from_dev(ZW_SEC_AUTO, BATTERY_LOW, sizeof BATTERY_LOW);
    TEST_ASSERT_EQUAL_UINT(2, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(0, zw_s2_node_stats(&panel, DEV)->decrypt_failures);
}

/* ZW-007: nonce reports without floods - Nonce Gets while the panel's Nonce Report still waits
 * to go get no second one; once it has gone, the next Nonce Get gets a new one. */
static void test_nonce_reports_without_floods(void)
{
    pair(ZW_S2_KEY_AUTH);
    uint8_t get[3];
    hold = true;
    for (uint8_t s = 10; s < 13; s++) {
        TEST_ASSERT_EQUAL_UINT(3, zw_s2_nonce_get_build(get, sizeof get, s));
        zw_s2_receive(&panel, now, DEV, get, sizeof get);
    }
    TEST_ASSERT_EQUAL_UINT(1, air_tail - air_head);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_NONCE_REPORT, air[air_head].b[1]);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_NONCE_SOS, air[air_head].b[3]);
    TEST_ASSERT_EQUAL_HEX8(0x05, air[air_head].opt);
    hold = false;
    pump();
    TEST_ASSERT_EQUAL_UINT(3, zw_s2_nonce_get_build(get, sizeof get, 13));
    hold = true;
    zw_s2_receive(&panel, now, DEV, get, sizeof get);
    TEST_ASSERT_EQUAL_UINT(1, air_tail - air_head);
    TEST_ASSERT_EQUAL_UINT(2, zw_s2_node_stats(&panel, DEV)->nonce_reports);
}

/* ZW-007, §6: a Nonce Report forgets the peer's last sequence number: a device that restarted
 * meanwhile may start its counter on that very number, and is not taken for a duplicate. */
static void test_nonce_report_forgets_last_seq(void)
{
    pair(ZW_S2_KEY_AUTH);
    uint8_t get[3];
    TEST_ASSERT_EQUAL_UINT(3, zw_s2_nonce_get_build(get, sizeof get, 0x40));
    zw_s2_receive(&panel, now, DEV, get, sizeof get);
    pump();
    zw_s2_receive(&panel, now, DEV, get, sizeof get); /* seq 0x40 again, after a restart */
    pump();
    TEST_ASSERT_EQUAL_UINT(2, zw_s2_node_stats(&panel, DEV)->nonce_reports);
    TEST_ASSERT_EQUAL_UINT(0, count_events(0, ZW_S2_EV_DUPLICATE));
}

/* ZW-007: the device restarts and loses its SPAN: the panel's next command cannot be decoded,
 * the device answers a Nonce Report within 500 ms, and the panel sends it once more on a new
 * SPAN. */
static void test_device_restart_panel_resends(void)
{
    pair(ZW_S2_KEY_AUTH);
    from_dev(ZW_SEC_AUTO, door_open, door_len);
    run_to(now + 1000u);
    dev_up(ZW_S2_KEY_AUTH);
    from_panel(ZW_SEC_AUTO, BASIC_GET, sizeof BASIC_GET, 9u);
    expect_got(1, 0, ZW_SEC_S2_AUTH, BASIC_GET, sizeof BASIC_GET);
    TEST_ASSERT_EQUAL_UINT(1, got_n[1]);
    TEST_ASSERT_EQUAL_UINT(1, zw_s2_node_stats(&panel, DEV)->resends);
    run_to(now + 1000u);
    TEST_ASSERT_EQUAL_UINT(1, done_n[0]);
    TEST_ASSERT_EQUAL_UINT32(9u, done[0][0].tag);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, done[0][0].r);
}

/* ZW-007: a frame the node cannot decode on a new SPAN either (here it holds another key) is
 * given up after the second Nonce Report: "the device could not decode it". */
static void test_not_decoded_twice(void)
{
    pair(ZW_S2_KEY_AUTH);
    from_dev(ZW_SEC_AUTO, door_open, door_len);
    run_to(now + 1000u);
    uint8_t other[16];
    memset(other, 0x77, sizeof other);
    TEST_ASSERT_TRUE(zw_s2_set_key(&dev, ZW_S2_KEY_AUTH, other));
    from_panel(ZW_SEC_AUTO, BASIC_GET, sizeof BASIC_GET, 5u);
    TEST_ASSERT_EQUAL_UINT(0, got_n[1]);
    TEST_ASSERT_EQUAL_UINT(1, done_n[0]);
    TEST_ASSERT_EQUAL_INT(ZW_S2_NOT_DECODED, done[0][0].r);
}

/* ZW-007: the node's Nonce Report never comes: the send fails NONCE_WAIT_MS (plus the
 * round trip) after its Nonce Get was delivered. */
static void test_no_nonce_report(void)
{
    pair(ZW_S2_KEY_AUTH);
    lose_dev = 1u;
    const uint64_t t0 = now;
    from_panel(ZW_SEC_AUTO, BASIC_GET, sizeof BASIC_GET, 6u);
    run_to(t0 + NONCE_WAIT_MS - 1u);
    TEST_ASSERT_EQUAL_UINT(0, done_n[0]);
    run_to(t0 + NONCE_WAIT_MS);
    TEST_ASSERT_EQUAL_UINT(1, done_n[0]);
    TEST_ASSERT_EQUAL_INT(ZW_S2_NO_NONCE, done[0][0].r);
}

/* ZW-007: sends to one node go one at a time, in order; a failed delivery fails its send. */
static void test_sends_in_order(void)
{
    pair(ZW_S2_KEY_AUTH);
    static const uint8_t A[] = {0x20, 0x02}, B[] = {0x80, 0x02}, C[] = {0x84, 0x05};
    static const uint8_t ITS[] = {0x80, 0x84}; /* ZW-009: supported, so encrypted (isCCSecure) */
    TEST_ASSERT_TRUE(zw_s2_set_secure_ccs(&panel, DEV, ITS, sizeof ITS));
    hold = true;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_s2_send(&panel, now, DEV, ZW_SEC_AUTO, A, 2, 1u));
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_s2_send(&panel, now, DEV, ZW_SEC_AUTO, B, 2, 2u));
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_s2_send(&panel, now, DEV, ZW_SEC_AUTO, C, 2, 3u));
    TEST_ASSERT_EQUAL_UINT(1, air_tail - air_head); /* the first one's Nonce Get only */
    hold = false;
    run_to(now + 3000u);
    expect_got(1, 0, ZW_SEC_S2_AUTH, A, 2);
    expect_got(1, 1, ZW_SEC_S2_AUTH, B, 2);
    expect_got(1, 2, ZW_SEC_S2_AUTH, C, 2);
    TEST_ASSERT_EQUAL_UINT(3, done_n[0]);
    for (unsigned i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_UINT32(i + 1u, done[0][i].tag);
        TEST_ASSERT_EQUAL_INT(ZW_S2_OK, done[0][i].r);
    }
    lose_panel = 1u;
    from_panel(ZW_SEC_AUTO, A, 2, 4u);
    TEST_ASSERT_EQUAL_INT(ZW_S2_FAILED, done[0][3].r);
}

/* ZW-009, §8 "Sending" (Z-Wave JS Driver.ts isCCSecure): a CC the S2 node supports - its NIF's
 * Z-Wave Plus Info too - and Basic go at its highest class; the refusals. */
static void test_send_choices_and_refusals(void)
{
    pair(ZW_S2_KEY_AUTH);
    from_panel(ZW_SEC_AUTO, PLUS_INFO_REPORT, sizeof PLUS_INFO_REPORT, 1u);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_CC, heard[0].b[0]); /* a Nonce Get first: it goes encrypted */
    expect_got(1, 0, ZW_SEC_S2_AUTH, PLUS_INFO_REPORT, sizeof PLUS_INFO_REPORT);
    run_to(now + 1000u);
    from_panel(ZW_SEC_AUTO, BASIC_GET, sizeof BASIC_GET, 2u);
    expect_got(1, 1, ZW_SEC_S2_AUTH, BASIC_GET, sizeof BASIC_GET);
    uint8_t big[40] = {0x20, 0x01}; /* Basic: encrypted, so it must fit with the S2 header */
    TEST_ASSERT_EQUAL_INT(ZW_S2_UNKNOWN_NODE, zw_s2_send(&panel, now, 9u, ZW_SEC_AUTO, BASIC_GET,
                                                         2, 0u));
    TEST_ASSERT_EQUAL_INT(ZW_S2_NO_KEY, zw_s2_send(&panel, now, DEV, ZW_SEC_S2_ACCESS, BASIC_GET,
                                                   2, 0u));
    TEST_ASSERT_EQUAL_INT(ZW_S2_NO_KEY, zw_s2_send(&panel, now, DEV, ZW_SEC_TEMP, BASIC_GET, 2,
                                                   0u));
    /* 46 bytes a frame: 4 + 34 + 8 fits, 35 does not */
    TEST_ASSERT_EQUAL_INT(ZW_S2_TOO_LONG, zw_s2_send(&panel, now, DEV, ZW_SEC_AUTO, big, 35, 0u));
    TEST_ASSERT_EQUAL_INT(ZW_S2_INVALID, zw_s2_send(&panel, now, DEV, ZW_SEC_AUTO, big, 0, 0u));
    run_to(now + 1000u); /* the Basic Get's 500 ms are over: every slot free */
    hold = true;
    for (uint32_t i = 0; i < ZW_S2_SENDS; i++) {
        TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_s2_send(&panel, now, DEV, ZW_SEC_AUTO, BASIC_GET, 2,
                                                   10u + i));
    }
    TEST_ASSERT_EQUAL_INT(ZW_S2_BUSY, zw_s2_send(&panel, now, DEV, ZW_SEC_AUTO, BASIC_GET, 2, 0u));
}

/* ---- Which frames the panel believes (§8) and the layers (command-classes.md §1) ---------- */

/* ZW-009: an unencrypted "door closed" from a secure sensor is dropped; so is a CC of its NIF
 * (Z-Wave JS isSecurityLevelTooLow: an S2 node needs its highest class for every command);
 * anything from a node with no class is accepted. */
static void test_unencrypted_door_closed_dropped(void)
{
    pair(ZW_S2_KEY_AUTH);
    zw_s2_receive(&panel, now, DEV, door_closed, door_len);
    TEST_ASSERT_EQUAL_UINT(0, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(1, count_events(0, ZW_S2_EV_WRONG_LEVEL));
    TEST_ASSERT_EQUAL_UINT(1, zw_s2_node_stats(&panel, DEV)->wrong_level);
    zw_s2_receive(&panel, now, DEV, PLUS_INFO_REPORT, sizeof PLUS_INFO_REPORT);
    TEST_ASSERT_EQUAL_UINT(0, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(2, zw_s2_node_stats(&panel, DEV)->wrong_level);
    TEST_ASSERT_TRUE(zw_s2_add_node(&panel, 3u, 0u, NIF, sizeof NIF)); /* included without S2 */
    zw_s2_receive(&panel, now, 3u, door_closed, door_len);
    TEST_ASSERT_EQUAL_UINT(1, got_n[0]);
    TEST_ASSERT_EQUAL_UINT16(3, got[0][0].rx.node);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, got[0][0].rx.security);
}

/* ZW-009 (Z-Wave JS Node.ts updateNodeInfo, before interviewStage NodeInfo: a node information
 * frame adds its CCs as supported): the primitive - a NIF only adds CCs to what goes encrypted; one listing fewer CCs removes none, so
 * nothing goes out in the clear because of it. */
static void test_nif_adds_never_removes(void)
{
    pair(ZW_S2_KEY_AUTH);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_AUTH, zw_s2_class_for(&panel, DEV, 0x5Eu)); /* its NIF */
    TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, zw_s2_class_for(&panel, DEV, 0x25u));   /* not listed */
    static const uint8_t FEWER[] = {0x9F};
    TEST_ASSERT_TRUE(zw_s2_add_nif_ccs(&panel, DEV, FEWER, sizeof FEWER));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_AUTH, zw_s2_class_for(&panel, DEV, 0x5Eu));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_AUTH, zw_s2_class_for(&panel, DEV, 0x6Cu));
    TEST_ASSERT_TRUE(zw_s2_add_nif_ccs(&panel, DEV, NULL, 0u)); /* an empty one: nothing */
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_AUTH, zw_s2_class_for(&panel, DEV, 0x5Eu));
    static const uint8_t MORE[] = {0x5E, 0x25};
    TEST_ASSERT_TRUE(zw_s2_add_nif_ccs(&panel, DEV, MORE, sizeof MORE));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_AUTH, zw_s2_class_for(&panel, DEV, 0x25u)); /* added */
    TEST_ASSERT_FALSE(zw_s2_add_nif_ccs(&panel, 9u, MORE, sizeof MORE)); /* not a known node */
}

/* ZW-009 (Z-Wave JS isCCConsideredSecure: Security 0's Nonce Get, Nonce Report, Scheme Get and
 * Scheme Report "Cannot be sent encapsulated" and are always accepted): from an S2 node, in the
 * clear, they are delivered; another Security 0 command in the clear (Commands Supported Get) is a
 * wrong level. */
static void test_s0_nonce_and_scheme_in_clear_from_s2_node(void)
{
    pair(ZW_S2_KEY_AUTH);
    static const uint8_t S0_NONCE_GET[] = {0x98, 0x40};
    static const uint8_t S0_NONCE_REPORT[] = {0x98, 0x80, 1, 2, 3, 4, 5, 6, 7, 8};
    static const uint8_t S0_SCHEME_GET[] = {0x98, 0x04, 0x00};
    static const uint8_t S0_SCHEME_REPORT[] = {0x98, 0x05, 0x00};
    static const uint8_t S0_SUPPORTED_GET[] = {0x98, 0x02};
    zw_s2_receive(&panel, now, DEV, S0_NONCE_GET, sizeof S0_NONCE_GET);
    zw_s2_receive(&panel, now, DEV, S0_NONCE_REPORT, sizeof S0_NONCE_REPORT);
    zw_s2_receive(&panel, now, DEV, S0_SCHEME_GET, sizeof S0_SCHEME_GET);
    zw_s2_receive(&panel, now, DEV, S0_SCHEME_REPORT, sizeof S0_SCHEME_REPORT);
    expect_got(0, 0, ZW_SEC_NONE, S0_NONCE_GET, sizeof S0_NONCE_GET);
    expect_got(0, 1, ZW_SEC_NONE, S0_NONCE_REPORT, sizeof S0_NONCE_REPORT);
    expect_got(0, 2, ZW_SEC_NONE, S0_SCHEME_GET, sizeof S0_SCHEME_GET);
    expect_got(0, 3, ZW_SEC_NONE, S0_SCHEME_REPORT, sizeof S0_SCHEME_REPORT);
    TEST_ASSERT_EQUAL_UINT(0u, count_events(0, ZW_S2_EV_WRONG_LEVEL));
    zw_s2_receive(&panel, now, DEV, S0_SUPPORTED_GET, sizeof S0_SUPPORTED_GET);
    TEST_ASSERT_EQUAL_UINT(4u, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(1u, count_events(0, ZW_S2_EV_WRONG_LEVEL));
}

/* ZW-042, the whole table (Z-Wave JS master 2026-10-03: the 147 commands whose class name ends in
 * "Get", a copy made by script from its CC sources, CC and command, in order): each is a Get, each
 * received broadcast or multicast reaches nothing (a Get is discarded; a Security 2 or Security 0
 * frame is not taken there either) - the Multi Channel rows too - the copy is strictly ascending,
 * and no other CC/command pair of all 65536 is a Get. */
static const uint8_t ZWJS_GETS[147][2] = {
    {0x20, 0x02}, {0x25, 0x02}, {0x26, 0x02}, {0x26, 0x06}, {0x2C, 0x02}, {0x2D, 0x02}, {0x30, 0x01}, {0x30, 0x02},
    {0x31, 0x04}, {0x32, 0x01}, {0x32, 0x03}, {0x33, 0x01}, {0x33, 0x03}, {0x40, 0x02}, {0x40, 0x04}, {0x42, 0x01},
    {0x42, 0x02}, {0x42, 0x05}, {0x43, 0x02}, {0x43, 0x04}, {0x43, 0x09}, {0x44, 0x02}, {0x44, 0x04}, {0x45, 0x02},
    {0x46, 0x02}, {0x46, 0x04}, {0x46, 0x07}, {0x47, 0x02}, {0x4C, 0x01}, {0x4C, 0x03}, {0x4E, 0x04}, {0x4E, 0x07},
    {0x4E, 0x09}, {0x4E, 0x0B}, {0x4E, 0x0E}, {0x59, 0x01}, {0x59, 0x03}, {0x59, 0x05}, {0x5B, 0x01}, {0x5B, 0x05},
    {0x5E, 0x01}, {0x60, 0x04}, {0x60, 0x07}, {0x60, 0x09}, {0x60, 0x0E}, {0x62, 0x02}, {0x62, 0x05}, {0x62, 0x07},
    {0x63, 0x02}, {0x63, 0x04}, {0x63, 0x06}, {0x63, 0x09}, {0x63, 0x0C}, {0x63, 0x0F}, {0x63, 0x11}, {0x64, 0x02},
    {0x64, 0x04}, {0x64, 0x06}, {0x64, 0x08}, {0x66, 0x02}, {0x66, 0x04}, {0x66, 0x07}, {0x6A, 0x01}, {0x6A, 0x03},
    {0x6B, 0x01}, {0x6B, 0x03}, {0x6B, 0x06}, {0x6B, 0x08}, {0x6B, 0x0B}, {0x6B, 0x0F}, {0x6C, 0x01}, {0x6D, 0x02},
    {0x6D, 0x04}, {0x6E, 0x01}, {0x6F, 0x02}, {0x6F, 0x04}, {0x6F, 0x07}, {0x70, 0x05}, {0x70, 0x08}, {0x70, 0x0A},
    {0x70, 0x0C}, {0x70, 0x0E}, {0x71, 0x01}, {0x71, 0x04}, {0x71, 0x07}, {0x72, 0x04}, {0x72, 0x06}, {0x73, 0x02},
    {0x73, 0x05}, {0x75, 0x02}, {0x75, 0x04}, {0x75, 0x07}, {0x75, 0x0A}, {0x76, 0x02}, {0x77, 0x02}, {0x77, 0x05},
    {0x79, 0x01}, {0x79, 0x03}, {0x79, 0x06}, {0x79, 0x09}, {0x7A, 0x01}, {0x7A, 0x03}, {0x7A, 0x05}, {0x7A, 0x0A},
    {0x80, 0x02}, {0x80, 0x04}, {0x81, 0x05}, {0x83, 0x01}, {0x83, 0x03}, {0x83, 0x06}, {0x83, 0x0B}, {0x83, 0x14},
    {0x83, 0x16}, {0x83, 0x18}, {0x83, 0x1B}, {0x83, 0x1D}, {0x83, 0x20}, {0x84, 0x05}, {0x84, 0x09}, {0x85, 0x02},
    {0x85, 0x05}, {0x85, 0x0B}, {0x86, 0x11}, {0x86, 0x13}, {0x86, 0x15}, {0x86, 0x17}, {0x87, 0x02}, {0x87, 0x04},
    {0x87, 0x06}, {0x89, 0x02}, {0x8A, 0x01}, {0x8A, 0x03}, {0x8A, 0x06}, {0x8B, 0x02}, {0x8E, 0x02}, {0x8E, 0x05},
    {0x90, 0x02}, {0x98, 0x02}, {0x98, 0x04}, {0x98, 0x40}, {0x98, 0xC1}, {0x9C, 0x01}, {0x9C, 0x03}, {0x9F, 0x01},
    {0x9F, 0x04}, {0x9F, 0x09}, {0x9F, 0x0D},
};

static void test_gets_table_whole(void)
{
    panel_up();
    TEST_ASSERT_TRUE(zw_s2_add_node(&panel, 3u, 0u, NIF, sizeof NIF)); /* no class */
    unsigned prev = 0u;
    for (size_t i = 0; i < 147u; i++) {
        const unsigned key = (unsigned)ZWJS_GETS[i][0] << 8 | (unsigned)ZWJS_GETS[i][1];
        TEST_ASSERT_TRUE_MESSAGE(i == 0u || key > prev, "not strictly ascending");
        prev = key;
        TEST_ASSERT_TRUE_MESSAGE(zw_s2_command_is_get(ZWJS_GETS[i], 2u), "a Get not in the table");
        const uint8_t g[3] = {ZWJS_GETS[i][0], ZWJS_GETS[i][1], 0x01};
        const bool kept = zw_s2_receive_group(&panel, now, 3u, g, sizeof g);
        if (g[0] != 0x98u && g[0] != 0x6Cu) { /* S0; a Supervision Get without the command it
                                                * wraps: malformed - counted, delivered to none */
            TEST_ASSERT_FALSE_MESSAGE(kept, "a Get received broadcast was kept");
        }
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, got_n[0], "a Get received broadcast was delivered");
    }
    unsigned gets = 0u;
    for (unsigned k = 0; k <= 0xFFFFu; k++) {
        const uint8_t c[2] = {(uint8_t)(k >> 8), (uint8_t)(k & 0xFFu)};
        gets += zw_s2_command_is_get(c, 2u) ? 1u : 0u;
    }
    TEST_ASSERT_EQUAL_UINT(147u, gets);
}

/* ZW-042 (Z-Wave JS shouldDiscardCC): received broadcast or multicast, a Get is discarded - the
 * panel's own Gets included (Firmware Update Meta Data's fragment Get, Version's Z-Wave Software
 * Get) - but a Multi Command is not looked inside (a multi-encapsulating CC): its Get and its
 * report are both delivered. */
static void test_group_get_and_multi_command(void)
{
    panel_up();
    TEST_ASSERT_TRUE(zw_s2_add_node(&panel, 3u, 0u, NIF, sizeof NIF)); /* no class */
    static const uint8_t FW_GET[] = {0x7A, 0x05, 0x01, 0x00, 0x01};
    static const uint8_t SW_GET[] = {0x86, 0x17};
    TEST_ASSERT_FALSE(zw_s2_receive_group(&panel, now, 3u, BASIC_GET, sizeof BASIC_GET));
    TEST_ASSERT_FALSE(zw_s2_receive_group(&panel, now, 3u, FW_GET, sizeof FW_GET));
    TEST_ASSERT_FALSE(zw_s2_receive_group(&panel, now, 3u, SW_GET, sizeof SW_GET));
    TEST_ASSERT_EQUAL_UINT(0u, got_n[0]);
    /* ZW-042, the table (Z-Wave JS's "Get" class names, APPENDIX of zw_s2.c): every listed Get
     * received broadcast is discarded; 31 01 and 31 03 (MultilevelSensorCCGetSupportedSensor /
     * Scale: not "...Get") and 60 0D (Multi Channel Command Encapsulation) are not Gets; nor are
     * reports and Sets */
    static const uint8_t SAMPLE_GETS[][2] = {
        {0x20, 0x02}, {0x26, 0x02}, {0x26, 0x06}, {0x60, 0x04}, {0x60, 0x07}, {0x60, 0x09},
        {0x60, 0x0E}, {0x62, 0x05}, {0x62, 0x07}, {0x63, 0x04}, {0x63, 0x0F}, {0x63, 0x11},
        {0x70, 0x08}, {0x70, 0x0A}, {0x70, 0x0C}, {0x70, 0x0E}, {0x73, 0x05}, {0x7A, 0x03},
        {0x7A, 0x0A}, {0x80, 0x04}, {0x83, 0x20}, {0x86, 0x17}, {0x98, 0xC1}, {0x9F, 0x0D},
    };
    for (size_t i = 0; i < sizeof SAMPLE_GETS / sizeof SAMPLE_GETS[0]; i++) {
        TEST_ASSERT_TRUE(zw_s2_command_is_get(SAMPLE_GETS[i], 2u));
        const uint8_t g[3] = {SAMPLE_GETS[i][0], SAMPLE_GETS[i][1], 0x01};
        const bool kept = zw_s2_receive_group(&panel, now, 3u, g, sizeof g);
        if (g[0] != 0x9F && g[0] != 0x98 && g[0] != 0x60) {
            TEST_ASSERT_FALSE_MESSAGE(kept, "a Get received broadcast was kept");
        }
    }
    TEST_ASSERT_EQUAL_UINT(0u, got_n[0]);
    static const uint8_t NOT_GETS[][2] = {
        {0x31, 0x01}, {0x31, 0x03}, {0x60, 0x0D}, {0x20, 0x01}, {0x20, 0x03}, {0x31, 0x05},
        {0x71, 0x05}, {0x80, 0x03}, {0x86, 0x12}, {0x9F, 0x0E}, {0x00, 0x00}, {0xFF, 0xFF},
    };
    for (size_t i = 0; i < sizeof NOT_GETS / sizeof NOT_GETS[0]; i++) {
        TEST_ASSERT_FALSE(zw_s2_command_is_get(NOT_GETS[i], 2u));
    }
    TEST_ASSERT_FALSE(zw_s2_command_is_get(NOT_GETS[0], 1u));
    static const uint8_t SENSOR_SUPPORTED_GET[] = {0x31, 0x01};
    TEST_ASSERT_TRUE(zw_s2_receive_group(&panel, now, 3u, SENSOR_SUPPORTED_GET, 2u));
    expect_got(0, 0, ZW_SEC_NONE, SENSOR_SUPPORTED_GET, 2u);
    got_n[0] = 0u;
    const uint8_t *const cmds[2] = {BASIC_GET, BATTERY_LOW};
    const size_t lens[2] = {sizeof BASIC_GET, sizeof BATTERY_LOW};
    uint8_t mc[32];
    const size_t n = zw_cc_wrap_multi_command(mc, sizeof mc, cmds, lens, 2u);
    TEST_ASSERT_TRUE(n > 0u);
    TEST_ASSERT_TRUE(zw_s2_receive_group(&panel, now, 3u, mc, n));
    expect_got(0, 0, ZW_SEC_NONE, BASIC_GET, sizeof BASIC_GET);
    expect_got(0, 1, ZW_SEC_NONE, BATTERY_LOW, sizeof BATTERY_LOW);
    TEST_ASSERT_TRUE(got[0][0].rx.multi_command);
    /* singlecast, the same Gets are the node's own questions */
    zw_s2_receive(&panel, now, 3u, SW_GET, sizeof SW_GET);
    expect_got(0, 2, ZW_SEC_NONE, SW_GET, sizeof SW_GET);
}

/* ZW-009 (Z-Wave JS: an accepted Security 2 Commands Supported Get reaches handleRequest, where
 * markAsAlive is): the manager answers it itself and says so (ZW_S2_EV_ANSWERED), so its owner
 * counts it as heard; in the clear from an S2 node it is accepted too (no answer). */
static void test_commands_supported_get_heard(void)
{
    pair(ZW_S2_KEY_AUTH);
    static const uint8_t CSG[] = {0x9F, 0x0D};
    from_dev(ZW_SEC_S2_AUTH, CSG, sizeof CSG);
    TEST_ASSERT_EQUAL_UINT(1u, count_events(0, ZW_S2_EV_ANSWERED));
    TEST_ASSERT_EQUAL_UINT(1u, got_n[1]); /* its Commands Supported Report */
    zw_s2_receive(&panel, now, DEV, CSG, sizeof CSG);
    pump();
    TEST_ASSERT_EQUAL_UINT(2u, count_events(0, ZW_S2_EV_ANSWERED));
    TEST_ASSERT_EQUAL_UINT(1u, got_n[1]);
    TEST_ASSERT_EQUAL_UINT(0u, count_events(0, ZW_S2_EV_WRONG_LEVEL));
}

/* ZW-009, ZW-017 (Z-Wave JS Security2CC.ts decryptSinglecast learns an unknown node's class by
 * decrypting): the panel restarted with its cache lost - the node's classes unknown, no SPAN - and
 * the node sends on its old SPAN. The panel answers with a Nonce Report, tries every key it holds
 * on the frame sent again, and marks the class that decrypts it granted: delivered at that class,
 * the other classes still undecided (the mark stays). Without the mark, a node with no class:
 * dropped (NO_KEY), no Nonce Report. */
static void test_unknown_classes_learnt_by_decrypting(void)
{
    pair(ZW_S2_KEY_AUTH | ZW_S2_KEY_UNAUTH);
    from_dev(ZW_SEC_S2_AUTH, door_open, door_len);
    expect_got(0, 0, ZW_SEC_S2_AUTH, door_open, door_len);
    run_to(now + 5000u); /* its send's grace over */
    /* the panel restarts: the device keeps its SPAN */
    panel_up();
    TEST_ASSERT_TRUE(zw_s2_add_node(&panel, DEV, 0u, NIF, sizeof NIF));
    TEST_ASSERT_TRUE(zw_s2_send(&dev, now, PANEL, ZW_SEC_S2_AUTH, door_closed, door_len, 0x100u) ==
                     ZW_S2_OK);
    pump();
    TEST_ASSERT_EQUAL_UINT(1u, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(1u, count_events(0, ZW_S2_EV_NO_KEY));
    TEST_ASSERT_EQUAL_UINT(0u, zw_s2_node_stats(&panel, DEV)->nonce_reports);
    run_to(now + 5000u);
    /* its classes marked unknown (the cache lost): learnt from the frame */
    TEST_ASSERT_TRUE(zw_s2_set_classes_unknown(&panel, DEV, true));
    TEST_ASSERT_FALSE(zw_s2_set_classes_unknown(&panel, 9u, true));
    from_dev(ZW_SEC_S2_AUTH, door_open, door_len);
    TEST_ASSERT_TRUE(zw_s2_node_stats(&panel, DEV)->nonce_reports >= 1u);
    expect_got(0, 1, ZW_SEC_S2_AUTH, door_open, door_len);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_AUTH, zw_s2_highest(&panel, DEV));
    TEST_ASSERT_TRUE(zw_s2_classes_unknown(&panel, DEV)); /* the others undecided */
    TEST_ASSERT_EQUAL_UINT8(ZW_S2_KEY_AUTH, zw_s2_granted(&panel, DEV));
    /* learnt: its own class from now on */
    run_to(now + 5000u);
    from_dev(ZW_SEC_S2_AUTH, door_closed, door_len);
    expect_got(0, 2, ZW_SEC_S2_AUTH, door_closed, door_len);
    TEST_ASSERT_EQUAL_UINT(0u, count_events(0, ZW_S2_EV_WRONG_LEVEL));
    /* the probe's decision ends the mark */
    TEST_ASSERT_TRUE(zw_s2_set_classes_unknown(&panel, DEV, true));
    TEST_ASSERT_TRUE(zw_s2_set_granted(&panel, DEV, ZW_S2_KEY_AUTH));
    TEST_ASSERT_FALSE(panel.peers[0].classes_unknown);
}

/* ZW-009: a node whose classes are unknown, on a panel holding no S2 key: nothing to try - its
 * Security 2 is dropped (NO_KEY), unanswered. */
static void test_unknown_classes_without_keys(void)
{
    memset(&keys, 0, sizeof keys);
    pair(ZW_S2_KEY_AUTH);
    memset(panel.keys, 0, sizeof panel.keys); /* the panel's keys absent */
    TEST_ASSERT_TRUE(zw_s2_add_node(&panel, DEV, 0u, NIF, sizeof NIF));
    TEST_ASSERT_TRUE(zw_s2_set_classes_unknown(&panel, DEV, true));
    TEST_ASSERT_TRUE(zw_s2_send(&dev, now, PANEL, ZW_SEC_S2_AUTH, door_open, door_len, 0x100u) ==
                     ZW_S2_OK);
    pump();
    TEST_ASSERT_EQUAL_UINT(0u, got_n[0]);
    TEST_ASSERT_TRUE(count_events(0, ZW_S2_EV_NO_KEY) >= 1u);
    /* unanswered: the only Nonce Report is the answer to its Nonce Get (it had no SPAN); the
     * encrypted frame itself draws none */
    TEST_ASSERT_TRUE(zw_s2_node_stats(&panel, DEV)->nonce_reports <= 1u);
    const uint32_t reports = zw_s2_node_stats(&panel, DEV)->nonce_reports;
    run_to(now + 5000u);
    TEST_ASSERT_TRUE(zw_s2_send(&dev, now, PANEL, ZW_SEC_S2_AUTH, door_closed, door_len, 0x101u) ==
                     ZW_S2_OK);
    pump();
    TEST_ASSERT_EQUAL_UINT(0u, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(reports, zw_s2_node_stats(&panel, DEV)->nonce_reports);
}

/* ZW-009 (Z-Wave JS Security2CC.ts decryptSinglecast tries every class whose hasSecurityClass is
 * not false; 05_Security.ts getHighestSecurityClass is the highest one granted; Driver.ts
 * isSecurityLevelTooLow then drops frames below it): a cache-lost node holding Access Control and
 * Unauthenticated whose first frame decrypts at Unauthenticated (its own, or forged by any holder
 * of that key) is not locked to it - its next frame at Access Control decrypts on a new SPAN, is
 * delivered at Access Control and raises its highest class, and a later Unauthenticated frame is
 * a wrong level. A class refused by the probe is no longer tried. */
static void test_unknown_classes_learnt_class_does_not_lock(void)
{
    pair(ZW_S2_KEY_ACCESS | ZW_S2_KEY_UNAUTH);
    TEST_ASSERT_TRUE(zw_s2_add_node(&panel, DEV, 0u, NIF, sizeof NIF));
    TEST_ASSERT_TRUE(zw_s2_set_classes_unknown(&panel, DEV, true));
    from_dev(ZW_SEC_S2_UNAUTH, door_open, door_len);
    expect_got(0, 0, ZW_SEC_S2_UNAUTH, door_open, door_len);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_UNAUTH, zw_s2_highest(&panel, DEV));
    run_to(now + 5000u);
    from_dev(ZW_SEC_S2_ACCESS, door_closed, door_len);
    expect_got(0, 1, ZW_SEC_S2_ACCESS, door_closed, door_len);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_ACCESS, zw_s2_highest(&panel, DEV));
    TEST_ASSERT_EQUAL_UINT8(ZW_S2_KEY_ACCESS | ZW_S2_KEY_UNAUTH, zw_s2_granted(&panel, DEV));
    TEST_ASSERT_EQUAL_UINT(0u, count_events(0, ZW_S2_EV_WRONG_LEVEL));
    run_to(now + 5000u);
    from_dev(ZW_SEC_S2_UNAUTH, door_open, door_len);
    TEST_ASSERT_EQUAL_UINT(2u, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(1u, count_events(0, ZW_S2_EV_WRONG_LEVEL));
    /* the probe refuses Access Control: no longer tried, nor sent at */
    run_to(now + 5000u);
    TEST_ASSERT_TRUE(zw_s2_set_class_result(&panel, DEV, ZW_S2_KEY_ACCESS, false));
    TEST_ASSERT_FALSE(zw_s2_set_class_result(&panel, DEV, ZW_S2_KEY_S0, false));
    TEST_ASSERT_EQUAL_UINT8(ZW_S2_KEY_ACCESS, zw_s2_refused(&panel, DEV));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_UNAUTH, zw_s2_highest(&panel, DEV));
    TEST_ASSERT_EQUAL_INT(ZW_S2_NO_KEY, zw_s2_send(&panel, now, DEV, ZW_SEC_S2_ACCESS, door_open,
                                                   door_len, 0x200u));
    /* Authenticated: undecided, its key held - a probe may send at it */
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_s2_send(&panel, now, DEV, ZW_SEC_S2_AUTH, door_open,
                                               door_len, 0x201u));
    pump();
}

/* ZW-009 (Z-Wave JS isSecurityLevelTooLow): before the node's Commands Supported Report, its
 * inclusion NIF - stale, listing Notification - lets no Notification Report in the clear. */
static void test_stale_nif_report_dropped(void)
{
    static const uint8_t STALE_NIF[] = {0x5E, 0x9F, 0x6C, 0x71, 0x80};
    pair(ZW_S2_KEY_AUTH);
    TEST_ASSERT_TRUE(zw_s2_add_node(&panel, DEV, ZW_S2_KEY_AUTH, STALE_NIF, sizeof STALE_NIF));
    zw_s2_receive(&panel, now, DEV, door_closed, door_len);
    zw_s2_receive(&panel, now, DEV, BATTERY_LOW, sizeof BATTERY_LOW);
    TEST_ASSERT_EQUAL_UINT(0, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(2, count_events(0, ZW_S2_EV_WRONG_LEVEL));
}

/* ZW-009 (Z-Wave JS: "CRC16 belongs outside of Security encapsulation"): a Security 2 frame inside
 * CRC-16 is decrypted and judged like a bare one - delivered at its class, its CRC-16 noted; a
 * plaintext report inside CRC-16 from the S2 node is a wrong level. */
static void test_crc16_around_s2(void)
{
    pair(ZW_S2_KEY_AUTH);
    crc_dev = 8u;
    from_dev(ZW_SEC_S2_AUTH, door_open, door_len);
    crc_dev = 0u;
    expect_got(0, 0, ZW_SEC_S2_AUTH, door_open, door_len);
    TEST_ASSERT_TRUE(got[0][0].rx.crc16);
    uint8_t f[32];
    const size_t n = zw_cc_wrap_crc16(f, sizeof f, door_closed, door_len);
    TEST_ASSERT_TRUE(n > 0u);
    zw_s2_receive(&panel, now, DEV, f, n);
    TEST_ASSERT_EQUAL_UINT(1, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(1, count_events(0, ZW_S2_EV_WRONG_LEVEL));
    /* a bare Security 2 command inside CRC-16 in the clear: taken, as Z-Wave JS does */
    static const uint8_t CSR[] = {0x9F, 0x0E, 0x71};
    const size_t m = zw_cc_wrap_crc16(f, sizeof f, CSR, sizeof CSR);
    zw_s2_receive(&panel, now, DEV, f, m);
    expect_got(0, 1, ZW_SEC_NONE, CSR, sizeof CSR);
}

/* ZW-009: a Supervision Get sent in the clear around a Notification Report is dropped (§8's
 * example): the inner CC counts. */
static void test_clear_supervision_around_secure_dropped(void)
{
    pair(ZW_S2_KEY_AUTH);
    uint8_t f[32];
    const size_t n = zw_cc_wrap_supervision(f, sizeof f, 3u, false, door_closed, door_len);
    TEST_ASSERT_TRUE(n > 0u);
    zw_s2_receive(&panel, now, DEV, f, n);
    TEST_ASSERT_EQUAL_UINT(0, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(1, count_events(0, ZW_S2_EV_WRONG_LEVEL));
}

/* ZW-009: a secure CC at a lower class than the node's highest is dropped; at its highest it is
 * delivered. */
static void test_lower_class_dropped(void)
{
    pair(ZW_S2_KEY_AUTH | ZW_S2_KEY_UNAUTH);
    from_dev(ZW_SEC_S2_UNAUTH, door_open, door_len);
    TEST_ASSERT_EQUAL_UINT(0, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(1, count_events(0, ZW_S2_EV_WRONG_LEVEL));
    run_to(now + 1000u);
    from_dev(ZW_SEC_S2_AUTH, door_open, door_len);
    expect_got(0, 0, ZW_SEC_S2_AUTH, door_open, door_len);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_AUTH, zw_s2_highest(&panel, DEV));
}

/* ZW-009: frames from a node the panel does not know are dropped. */
static void test_unknown_node_dropped(void)
{
    pair(ZW_S2_KEY_AUTH);
    zw_s2_receive(&panel, now, 9u, PLUS_INFO_REPORT, sizeof PLUS_INFO_REPORT);
    uint8_t get[3];
    TEST_ASSERT_EQUAL_UINT(3, zw_s2_nonce_get_build(get, sizeof get, 1));
    zw_s2_receive(&panel, now, 9u, get, sizeof get);
    TEST_ASSERT_EQUAL_UINT(0, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(0, air_tail); /* not even a Nonce Report */
    TEST_ASSERT_EQUAL_UINT(2, count_events(0, ZW_S2_EV_UNKNOWN_NODE));
}

/* command-classes.md §1: S2 is only the outermost layer - an encapsulation inside Multi Channel
 * is dropped, encrypted or in the clear. */
static void test_s2_inside_multichannel_dropped(void)
{
    pair(ZW_S2_KEY_AUTH);
    static const uint8_t FAKE_S2[] = {0x9F, 0x03, 0x20, 0x00, 0x71, 0x05, 0x00, 0x00,
                                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t f[40];
    const size_t n = zw_cc_wrap_multi_channel(f, sizeof f, 1u, 0u, false, FAKE_S2, sizeof FAKE_S2);
    TEST_ASSERT_TRUE(n > 0u);
    from_dev(ZW_SEC_S2_AUTH, f, n);
    TEST_ASSERT_EQUAL_UINT(0, got_n[0]);
    zw_s2_receive(&panel, now, DEV, f, n);
    TEST_ASSERT_EQUAL_UINT(0, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(1, count_events(0, ZW_S2_EV_MALFORMED));
    TEST_ASSERT_EQUAL_UINT(1, count_events(0, ZW_S2_EV_WRONG_LEVEL)); /* ZW-009: the level first */
    /* a Security 2 command that is not an encapsulation is a command like any: an endpoint's
     * Commands Supported Report */
    static const uint8_t CSR[] = {0x9F, 0x0E, 0x71};
    const size_t m = zw_cc_wrap_multi_channel(f, sizeof f, 1u, 0u, false, CSR, sizeof CSR);
    run_to(now + 1000u);
    from_dev(ZW_SEC_S2_AUTH, f, m);
    expect_got(0, 0, ZW_SEC_S2_AUTH, CSR, sizeof CSR);
    TEST_ASSERT_TRUE(got[0][0].rx.multi_channel);
}

/* command-classes.md §1: every other layer appears at most once - Supervision inside
 * Supervision, Multi Channel inside Multi Channel are dropped. */
static void test_repeated_layer_dropped(void)
{
    pair(ZW_S2_KEY_AUTH);
    uint8_t in[24], f[32];
    size_t n = zw_cc_wrap_supervision(in, sizeof in, 1u, false, door_open, door_len);
    n = zw_cc_wrap_supervision(f, sizeof f, 2u, false, in, n);
    TEST_ASSERT_TRUE(n > 0u);
    from_dev(ZW_SEC_S2_AUTH, f, n);
    TEST_ASSERT_EQUAL_UINT(0, got_n[0]);
    run_to(now + 1000u);
    n = zw_cc_wrap_multi_channel(in, sizeof in, 1u, 0u, false, BATTERY_LOW, sizeof BATTERY_LOW);
    n = zw_cc_wrap_multi_channel(f, sizeof f, 2u, 0u, false, in, n);
    from_dev(ZW_SEC_S2_AUTH, f, n);
    TEST_ASSERT_EQUAL_UINT(0, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(2, count_events(0, ZW_S2_EV_MALFORMED));
}

/* command-classes.md §1: the normal chain S2 -> Supervision -> Multi Channel -> the report is
 * accepted at S2's class, with the session and the endpoint noted. */
static void test_s2_supervision_multichannel_accepted(void)
{
    pair(ZW_S2_KEY_AUTH);
    uint8_t in[24], f[32];
    size_t n = zw_cc_wrap_multi_channel(in, sizeof in, 2u, 0u, false, door_open, door_len);
    n = zw_cc_wrap_supervision(f, sizeof f, 5u, true, in, n);
    TEST_ASSERT_TRUE(n > 0u);
    from_dev(ZW_SEC_S2_AUTH, f, n);
    expect_got(0, 0, ZW_SEC_S2_AUTH, door_open, door_len);
    const zw_s2_rx_t *rx = &got[0][0].rx;
    TEST_ASSERT_TRUE(rx->supervised);
    TEST_ASSERT_EQUAL_UINT8(5, rx->supervision_session);
    TEST_ASSERT_TRUE(rx->supervision_updates);
    TEST_ASSERT_TRUE(rx->multi_channel);
    TEST_ASSERT_EQUAL_UINT8(2, rx->source_endpoint);
    TEST_ASSERT_EQUAL_UINT8(0, rx->destination_endpoint);
    TEST_ASSERT_FALSE(rx->crc16 || rx->multi_command);
}

/* command-classes.md §1: a Multi Command hands over each command in order; CRC-16 is checked
 * and noted; Transport Service and S0 encapsulation are dropped (v1, phase 3). ZW-009: S0
 * encapsulation in the clear from an S2 node is a wrong level; a Transport Service segment is not
 * supported, never a wrong level (Z-Wave JS Driver.ts joins the segments before
 * isSecurityLevelTooLow and judges only the joined command); a CRC-16 frame whose checksum fails is
 * malformed, never a wrong level (Z-Wave JS: a decode error in CommandClass.parse, before any
 * security check). */
static void test_multi_command_crc_and_unsupported(void)
{
    pair(ZW_S2_KEY_AUTH);
    const uint8_t *cmds[2] = {door_open, BATTERY_LOW};
    const size_t lens[2] = {door_len, sizeof BATTERY_LOW};
    uint8_t f[32];
    size_t n = zw_cc_wrap_multi_command(f, sizeof f, cmds, lens, 2u);
    TEST_ASSERT_TRUE(n > 0u);
    from_dev(ZW_SEC_S2_AUTH, f, n);
    expect_got(0, 0, ZW_SEC_S2_AUTH, door_open, door_len);
    expect_got(0, 1, ZW_SEC_S2_AUTH, BATTERY_LOW, sizeof BATTERY_LOW);
    TEST_ASSERT_TRUE(got[0][0].rx.multi_command && got[0][1].rx.multi_command);
    run_to(now + 1000u);
    n = zw_cc_wrap_crc16(f, sizeof f, BATTERY_LOW, sizeof BATTERY_LOW);
    from_dev(ZW_SEC_S2_AUTH, f, n);
    expect_got(0, 2, ZW_SEC_S2_AUTH, BATTERY_LOW, sizeof BATTERY_LOW);
    TEST_ASSERT_TRUE(got[0][2].rx.crc16);
    static const uint8_t TS[] = {0x55, 0xC0, 0x10, 0x00, 0x00};
    static const uint8_t S0[] = {0x98, 0x81, 1, 2, 3, 4, 5, 6, 7, 8, 0x10, 0x11, 0x01,
                                 1, 2, 3, 4, 5, 6, 7, 8};
    const zw_s2_node_stats_t before = *zw_s2_node_stats(&panel, DEV);
    zw_s2_receive(&panel, now, DEV, TS, sizeof TS);
    TEST_ASSERT_EQUAL_UINT(before.unsupported + 1u, zw_s2_node_stats(&panel, DEV)->unsupported);
    TEST_ASSERT_EQUAL_UINT(before.wrong_level, zw_s2_node_stats(&panel, DEV)->wrong_level);
    zw_s2_receive(&panel, now, DEV, S0, sizeof S0);
    TEST_ASSERT_EQUAL_UINT(3, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(1, count_events(0, ZW_S2_EV_UNSUPPORTED));
    TEST_ASSERT_EQUAL_UINT(1, count_events(0, ZW_S2_EV_WRONG_LEVEL));
    uint8_t bad[16];
    const size_t bn = zw_cc_wrap_crc16(bad, sizeof bad, BATTERY_LOW, sizeof BATTERY_LOW);
    TEST_ASSERT_TRUE(bn > 2u);
    bad[bn - 1u] = (uint8_t)(bad[bn - 1u] ^ 0x5Au); /* its checksum fails */
    const uint32_t malformed = zw_s2_node_stats(&panel, DEV)->malformed;
    zw_s2_receive(&panel, now, DEV, bad, bn);
    TEST_ASSERT_EQUAL_UINT(malformed + 1u, zw_s2_node_stats(&panel, DEV)->malformed);
    TEST_ASSERT_EQUAL_UINT(1, count_events(0, ZW_S2_EV_WRONG_LEVEL));
    TEST_ASSERT_EQUAL_UINT(3, got_n[0]);
    TEST_ASSERT_TRUE(zw_s2_add_node(&panel, 3u, 0u, NIF, sizeof NIF)); /* no class */
    zw_s2_receive(&panel, now, 3u, TS, sizeof TS);
    zw_s2_receive(&panel, now, 3u, S0, sizeof S0);
    TEST_ASSERT_EQUAL_UINT(3, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(3, count_events(0, ZW_S2_EV_UNSUPPORTED));
    /* S0 encapsulation inside S2 is not unsupported but against §1: security only outermost */
    run_to(now + 1000u);
    from_dev(ZW_SEC_S2_AUTH, S0, sizeof S0);
    TEST_ASSERT_EQUAL_UINT(3, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(3, count_events(0, ZW_S2_EV_UNSUPPORTED));
    TEST_ASSERT_EQUAL_UINT(2, count_events(0, ZW_S2_EV_MALFORMED)); /* + the bad CRC-16 */
    /* ZW-009 (TransportServiceCC.ts isTransportServiceEncapsulation: only First and Subsequent
     * Segment; Driver.ts isSecurityLevelTooLow judges the rest): Segment Request, Complete and
     * Wait in the clear from an S2 node are a wrong level, not "not supported" */
    static const uint8_t TS_CONTROL[3][3] = {{0x55, 0xC8, 0x10}, {0x55, 0xE8, 0x10},
                                             {0x55, 0xF0, 0x01}};
    const zw_s2_node_stats_t ts = *zw_s2_node_stats(&panel, DEV);
    for (size_t i = 0; i < 3u; i++) {
        zw_s2_receive(&panel, now, DEV, TS_CONTROL[i], sizeof TS_CONTROL[i]);
    }
    TEST_ASSERT_EQUAL_UINT(ts.wrong_level + 3u, zw_s2_node_stats(&panel, DEV)->wrong_level);
    TEST_ASSERT_EQUAL_UINT(ts.unsupported, zw_s2_node_stats(&panel, DEV)->unsupported);
    TEST_ASSERT_EQUAL_UINT(3, got_n[0]);
}

/* ---- Commands Supported (§9) ---------------------------------------------------------------- */

/* §9: asked at the node's highest class, the panel lists what it supports securely; at a lower
 * S2 class, an empty list at that class; in the clear, nothing. ZW-009 (Security2CC.ts: only the
 * interview's answer is the secure list): a node's own report, even at its highest class, is
 * handed over and sets nothing; the interview sets the list (zw_s2_set_secure_ccs). */
static void test_commands_supported(void)
{
    pair(ZW_S2_KEY_AUTH | ZW_S2_KEY_UNAUTH);
    static const uint8_t GET[] = {0x9F, 0x0D};
    static const uint8_t FULL[] = {0x9F, 0x0E, 0x86, 0x72, 0x85, 0x59, 0x8E, 0x5A};
    static const uint8_t EMPTY[] = {0x9F, 0x0E};
    from_dev(ZW_SEC_S2_AUTH, GET, sizeof GET);
    expect_got(1, 0, ZW_SEC_S2_AUTH, FULL, sizeof FULL);
    TEST_ASSERT_EQUAL_UINT(0, got_n[0]); /* answered, not handed over */
    run_to(now + 1000u);
    from_dev(ZW_SEC_S2_UNAUTH, GET, sizeof GET);
    expect_got(1, 1, ZW_SEC_S2_UNAUTH, EMPTY, sizeof EMPTY);
    run_to(now + 1000u);
    const size_t before = heard_n;
    zw_s2_receive(&panel, now, DEV, GET, sizeof GET);
    pump();
    TEST_ASSERT_EQUAL_UINT(before, heard_n);
    TEST_ASSERT_EQUAL_UINT(0, done_n[0]); /* the panel's own answers end silently */
    /* the node's report: Notification and Battery securely. ZW-009 (Z-Wave JS
     * isSecurityLevelTooLow): from an S2 node nothing but a bare Security 2 command comes in the
     * clear - neither Version nor Z-Wave Plus Info (its NIF's); a bare Commands Supported Report
     * in the clear is delivered but never becomes its secure list */
    static const uint8_t ITS[] = {0x9F, 0x0E, 0x71, 0x80};
    static const uint8_t VERSION_GET[] = {0x86, 0x11};
    static const uint8_t PLUS_GET[] = {0x5E, 0x01};
    zw_s2_receive(&panel, now, DEV, VERSION_GET, sizeof VERSION_GET);
    TEST_ASSERT_EQUAL_UINT(0, got_n[0]);
    from_dev(ZW_SEC_S2_AUTH, ITS, sizeof ITS);
    expect_got(0, 0, ZW_SEC_S2_AUTH, ITS, sizeof ITS);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, zw_s2_class_for(&panel, DEV, 0x80u)); /* not by itself */
    TEST_ASSERT_TRUE(zw_s2_set_secure_ccs(&panel, DEV, &ITS[2], sizeof ITS - 2u));
    zw_s2_receive(&panel, now, DEV, VERSION_GET, sizeof VERSION_GET);
    TEST_ASSERT_EQUAL_UINT(1, got_n[0]);
    zw_s2_receive(&panel, now, DEV, PLUS_GET, sizeof PLUS_GET);
    TEST_ASSERT_EQUAL_UINT(1, got_n[0]);
    zw_s2_receive(&panel, now, DEV, door_open, door_len);
    TEST_ASSERT_EQUAL_UINT(1, got_n[0]);
    static const uint8_t BARE_CSR[] = {0x9F, 0x0E, 0x71};
    zw_s2_receive(&panel, now, DEV, BARE_CSR, sizeof BARE_CSR);
    expect_got(0, 1, ZW_SEC_NONE, BARE_CSR, sizeof BARE_CSR);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_AUTH, zw_s2_class_for(&panel, DEV, 0x80u)); /* list kept */
}

/* ZW-009 (security.md §8, Z-Wave JS Driver.ts isSecurityLevelTooLow and isCCSecure): an S2
 * node's commands count only at its highest class, whatever its lists say. A plaintext Binary
 * Sensor Report is dropped, so is the same at a lower S2 class and inside Supervision; at its
 * highest class it is delivered. A CC its NIF lists is dropped in the clear too, bare and inside
 * Supervision. Sending: supported CCs (NIF or report) and Basic at its class, an unsupported CC
 * and a NOP in the clear. A node with no class is unchanged. */
static void test_unlisted_cc_below_class_dropped(void)
{
    static const uint8_t BINARY_OPEN[] = {0x30, 0x03, 0xFF, 0x0A};
    static const uint8_t SECURE_CCS[] = {0x71, 0x80};
    pair(ZW_S2_KEY_AUTH | ZW_S2_KEY_UNAUTH);
    TEST_ASSERT_TRUE(zw_s2_set_secure_ccs(&panel, DEV, SECURE_CCS, sizeof SECURE_CCS));
    zw_s2_receive(&panel, now, DEV, BINARY_OPEN, sizeof BINARY_OPEN);
    TEST_ASSERT_EQUAL_UINT(0, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(1, count_events(0, ZW_S2_EV_WRONG_LEVEL));
    TEST_ASSERT_EQUAL_UINT(1, zw_s2_node_stats(&panel, DEV)->wrong_level);
    from_dev(ZW_SEC_S2_UNAUTH, BINARY_OPEN, sizeof BINARY_OPEN);
    TEST_ASSERT_EQUAL_UINT(0, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(2, count_events(0, ZW_S2_EV_WRONG_LEVEL));
    run_to(now + 1000u);
    uint8_t f[32];
    size_t n = zw_cc_wrap_supervision(f, sizeof f, 4u, false, BINARY_OPEN, sizeof BINARY_OPEN);
    TEST_ASSERT_TRUE(n > 0u);
    zw_s2_receive(&panel, now, DEV, f, n);
    TEST_ASSERT_EQUAL_UINT(0, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(3, count_events(0, ZW_S2_EV_WRONG_LEVEL));
    /* at its highest class: delivered */
    from_dev(ZW_SEC_S2_AUTH, BINARY_OPEN, sizeof BINARY_OPEN);
    expect_got(0, 0, ZW_SEC_S2_AUTH, BINARY_OPEN, sizeof BINARY_OPEN);
    run_to(now + 1000u);
    from_dev(ZW_SEC_S2_AUTH, door_open, door_len);
    expect_got(0, 1, ZW_SEC_S2_AUTH, door_open, door_len);
    /* its NIF's CCs: dropped in the clear, bare and inside Supervision (6C, in its NIF) */
    zw_s2_receive(&panel, now, DEV, PLUS_INFO_REPORT, sizeof PLUS_INFO_REPORT);
    n = zw_cc_wrap_supervision(f, sizeof f, 5u, false, PLUS_INFO_REPORT, sizeof PLUS_INFO_REPORT);
    TEST_ASSERT_TRUE(n > 0u);
    zw_s2_receive(&panel, now, DEV, f, n);
    TEST_ASSERT_EQUAL_UINT(2, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(5, count_events(0, ZW_S2_EV_WRONG_LEVEL));
    /* sending (isCCSecure): supported or Basic at its class, unsupported or a NOP in the clear */
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_AUTH, zw_s2_class_for(&panel, DEV, 0x5Eu));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_AUTH, zw_s2_class_for(&panel, DEV, 0x20u));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S2_AUTH, zw_s2_class_for(&panel, DEV, 0x71u));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, zw_s2_class_for(&panel, DEV, 0x30u));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, zw_s2_class_for(&panel, DEV, 0x00u));
    /* a node with no class: its plaintext frames as before, and nothing is sent encrypted */
    TEST_ASSERT_TRUE(zw_s2_add_node(&panel, 3u, 0u, NIF, sizeof NIF));
    zw_s2_receive(&panel, now, 3u, BINARY_OPEN, sizeof BINARY_OPEN);
    TEST_ASSERT_EQUAL_UINT(3, got_n[0]);
    TEST_ASSERT_EQUAL_UINT16(3, got[0][2].rx.node);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, got[0][2].rx.security);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, zw_s2_class_for(&panel, 3u, 0x30u));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, zw_s2_class_for(&panel, 3u, 0x20u));
}

/* ZW-009 (Driver.ts isSecurityLevelTooLow, S0 highest - unit level, through the rule): only a CC
 * its secure list names needs S0; Security 0's own commands and an unlisted CC are taken in the
 * clear. Sending (isCCSecure): Basic and its listed CCs would go at S0 - for which the panel has
 * no key (phase 3). */
static void test_s0_node_rule(void)
{
    static const uint8_t LISTED[] = {0x71};
    static const uint8_t BASIC_SET[] = {0x20, 0x01, 0xFF};
    static const uint8_t S0_NONCE_GET[] = {0x98, 0x40};
    pair(ZW_S2_KEY_AUTH);
    TEST_ASSERT_TRUE(zw_s2_add_node(&panel, 4u, ZW_S2_KEY_S0, NIF, sizeof NIF));
    TEST_ASSERT_TRUE(zw_s2_set_secure_ccs(&panel, 4u, LISTED, sizeof LISTED));
    zw_s2_receive(&panel, now, 4u, door_closed, door_len);
    TEST_ASSERT_EQUAL_UINT(0, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(1, count_events(0, ZW_S2_EV_WRONG_LEVEL));
    zw_s2_receive(&panel, now, 4u, BASIC_SET, sizeof BASIC_SET);
    expect_got(0, 0, ZW_SEC_NONE, BASIC_SET, sizeof BASIC_SET);
    zw_s2_receive(&panel, now, 4u, S0_NONCE_GET, sizeof S0_NONCE_GET);
    expect_got(0, 1, ZW_SEC_NONE, S0_NONCE_GET, sizeof S0_NONCE_GET);
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S0, zw_s2_class_for(&panel, 4u, 0x20u));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_S0, zw_s2_class_for(&panel, 4u, 0x71u));
    TEST_ASSERT_EQUAL_INT(ZW_SEC_NONE, zw_s2_class_for(&panel, 4u, 0x5Eu));
}

/* ---- The key exchange's part (§5) --------------------------------------------------------- */

static void temp_keys(void)
{
    uint8_t k[16], ps[32];
    memset(k, 0xA1, sizeof k);
    memset(ps, 0xB2, sizeof ps);
    TEST_ASSERT_TRUE(zw_s2_set_temp_key(&panel, DEV, k, ps));
    TEST_ASSERT_TRUE(zw_s2_set_temp_key(&dev, PANEL, k, ps));
}

/* ZW-009: a node being bootstrapped is not judged on its level (Z-Wave JS
 * isSecurityLevelTooLow: its class is Temporary or not yet known) - a report under the temporary
 * key is delivered at ZW_SEC_TEMP, and so is the KEX Set echo. */
static void test_temporary_key(void)
{
    pair(0u);
    temp_keys();
    from_dev(ZW_SEC_TEMP, door_open, door_len);
    expect_got(0, 0, ZW_SEC_TEMP, door_open, door_len);
    TEST_ASSERT_EQUAL_UINT(0, count_events(0, ZW_S2_EV_WRONG_LEVEL));
    run_to(now + 1000u);
    static const uint8_t ECHO[] = {0x9F, 0x06, 0x01, 0x02, 0x01, 0x02};
    from_dev(ZW_SEC_TEMP, ECHO, sizeof ECHO);
    expect_got(0, 1, ZW_SEC_TEMP, ECHO, sizeof ECHO);
    /* deleted: frames under it no longer decrypt (and no class: dropped without a nonce) */
    zw_s2_clear_temp_key(&panel);
    TEST_ASSERT_EQUAL_INT(ZW_S2_NO_KEY, zw_s2_send(&panel, now, DEV, ZW_SEC_TEMP, ECHO, 6, 0u));
}

/* ZW-007: the Network Key Verify comes on a new SPAN under the key just sent: the panel tries the
 * temporary key, then the class being exchanged. */
static void test_exchange_class_verify(void)
{
    pair(0u);
    temp_keys();
    zw_s2_set_exchange_class(&panel, ZW_SEC_S2_AUTH);
    TEST_ASSERT_TRUE(zw_s2_set_key(&dev, ZW_S2_KEY_AUTH, keys.s2_auth));
    TEST_ASSERT_TRUE(zw_s2_set_granted(&dev, PANEL, ZW_S2_KEY_AUTH));
    static const uint8_t VERIFY[] = {0x9F, 0x0B};
    from_dev(ZW_SEC_S2_AUTH, VERIFY, sizeof VERIFY);
    expect_got(0, 0, ZW_SEC_S2_AUTH, VERIFY, sizeof VERIFY);
}

/* ZW-007, §7: an S2 frame from a node with no class and no temporary key is dropped - no Nonce
 * Report either. */
static void test_no_key_no_nonce(void)
{
    pair(ZW_S2_KEY_AUTH);
    from_dev(ZW_SEC_AUTO, door_open, door_len);
    const air_t last = heard[heard_n - 1u];
    TEST_ASSERT_TRUE(zw_s2_set_granted(&panel, DEV, 0u));
    const size_t before = air_tail;
    zw_s2_receive(&panel, now, DEV, last.b, last.len);
    TEST_ASSERT_EQUAL_UINT(before, air_tail);
    TEST_ASSERT_EQUAL_UINT(1, count_events(0, ZW_S2_EV_NO_KEY));
}

/* ZW-007: a decryption failure on a SPAN extension built on the panel's receiver EI is told
 * apart (the key exchange's wrong-PIN rule counts these); a frame on a SPAN the panel does not
 * have is a plain failure. */
static void test_failure_kinds(void)
{
    pair(ZW_S2_KEY_AUTH);
    uint8_t other[16];
    memset(other, 0x78, sizeof other);
    TEST_ASSERT_TRUE(zw_s2_set_key(&dev, ZW_S2_KEY_AUTH, other));
    from_dev(ZW_SEC_AUTO, door_open, door_len);
    TEST_ASSERT_EQUAL_UINT(0, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(2, count_events(0, ZW_S2_EV_DECRYPT_FAILED_SPAN)); /* and its resend */
    TEST_ASSERT_EQUAL_UINT(1, done_n[1]);
    TEST_ASSERT_EQUAL_INT(ZW_S2_NOT_DECODED, done[1][0].r);
}

/* ---- Transmissions (serial-api.md §6.12) --------------------------------------------------- */

/* ZW-037, ZW-007: a frame the radio did not deliver goes again as a new frame - a new sequence
 * number and the SPAN's next nonce (the same command encrypts differently) - and the device takes
 * it once, within its nonce tolerance, without a resync. Without the owner's attempts function a
 * frame goes once, as before. */
static void test_resend_new_nonce_and_sequence(void)
{
    pair(ZW_S2_KEY_AUTH);
    zw_s2_set_attempts(&panel, panel_attempts);
    from_dev(ZW_SEC_AUTO, door_open, door_len);
    run_to(now + 1000u); /* a SPAN in use */
    const size_t from = air_tail;
    lose_panel = 1u;
    from_panel(ZW_SEC_AUTO, BASIC_GET, sizeof BASIC_GET, 7u);
    TEST_ASSERT_EQUAL_UINT(2, panel_frames_since(from));
    const air_t *a = &air[from % AIR];
    const air_t *b = &air[(from + 1u) % AIR];
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_CC, a->b[0]);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_MESSAGE_ENCAPSULATION, a->b[1]);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_MESSAGE_ENCAPSULATION, b->b[1]);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(a->b[2] + 1u), b->b[2]); /* the next sequence number */
    TEST_ASSERT_EQUAL_UINT(a->len, b->len);
    TEST_ASSERT_FALSE(memcmp(a->b, b->b, a->len) == 0);      /* another nonce, another frame */
    TEST_ASSERT_NOT_EQUAL(a->tag, b->tag);
    expect_got(1, 0, ZW_SEC_S2_AUTH, BASIC_GET, sizeof BASIC_GET);
    TEST_ASSERT_EQUAL_UINT(1, got_n[1]);
    run_to(now + 1000u); /* the resend window */
    TEST_ASSERT_EQUAL_UINT(1, done_n[0]);
    TEST_ASSERT_EQUAL_UINT32(7u, done[0][0].tag);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, done[0][0].r);
    TEST_ASSERT_EQUAL_UINT(1, zw_s2_node_stats(&panel, DEV)->retries);
    TEST_ASSERT_EQUAL_UINT(0, zw_s2_node_stats(&panel, DEV)->resends);
    TEST_ASSERT_EQUAL_UINT(0, zw_s2_node_stats(&dev, PANEL)->decrypt_failures);
    TEST_ASSERT_EQUAL_UINT16(DEV, asked_node);
    TEST_ASSERT_EQUAL_UINT32(7u, asked_tag); /* the owner's tag, not the frame's */
    /* no attempts function: once */
    zw_s2_set_attempts(&panel, NULL);
    lose_panel = 1u;
    from_panel(ZW_SEC_AUTO, BASIC_GET, sizeof BASIC_GET, 8u);
    TEST_ASSERT_EQUAL_UINT(3, panel_frames_since(from));
    TEST_ASSERT_EQUAL_INT(ZW_S2_FAILED, done[0][1].r);
}

/* ZW-037: three transmissions in all - two lost are bridged by the third, three lost fail the
 * send; a lost Nonce Get goes again the same way (a new one, on a new sequence number). */
static void test_three_noacks_fail(void)
{
    pair(ZW_S2_KEY_AUTH);
    zw_s2_set_attempts(&panel, panel_attempts);
    size_t from = air_tail;
    lose_panel = 2u; /* the first two Nonce Gets */
    from_panel(ZW_SEC_AUTO, BASIC_GET, sizeof BASIC_GET, 1u);
    run_to(now + 1000u);
    TEST_ASSERT_EQUAL_UINT(4, panel_frames_since(from)); /* 3 Nonce Gets, the command */
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_NONCE_GET, air[(from + 2u) % AIR].b[1]);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(air[from % AIR].b[2] + 2u), air[(from + 2u) % AIR].b[2]);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, done[0][0].r);
    TEST_ASSERT_EQUAL_UINT(1, got_n[1]);
    from = air_tail;
    lose_panel = 3u;
    from_panel(ZW_SEC_AUTO, BASIC_GET, sizeof BASIC_GET, 2u);
    run_to(now + 1000u);
    TEST_ASSERT_EQUAL_UINT(3, panel_frames_since(from));
    TEST_ASSERT_EQUAL_UINT(2, done_n[0]);
    TEST_ASSERT_EQUAL_INT(ZW_S2_FAILED, done[0][1].r);
    TEST_ASSERT_EQUAL_UINT(1, got_n[1]);
    TEST_ASSERT_EQUAL_UINT(4, zw_s2_node_stats(&panel, DEV)->retries);
    /* the next send to it starts afresh */
    from_panel(ZW_SEC_AUTO, BASIC_GET, sizeof BASIC_GET, 3u);
    run_to(now + 1000u);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, done[0][2].r);
    TEST_ASSERT_EQUAL_UINT(2, got_n[1]);
}

/* ZW-037: only the radio's own failures go again - NoAck, Fail after time on the air, NoRoute.
 * A jammed module (Fail with 0 ticks) and an end that did not parse fail the send at once, as
 * before (the jammed module's own handling is elsewhere). */
static void test_which_ends_are_sent_again(void)
{
    pair(ZW_S2_KEY_AUTH);
    zw_s2_set_attempts(&panel, panel_attempts);
    from_dev(ZW_SEC_AUTO, door_open, door_len);
    run_to(now + 1000u);
    static const zw_s2_tx_t AGAIN[] = {ZW_S2_TX_NO_ACK, ZW_S2_TX_FAIL, ZW_S2_TX_NO_ROUTE};
    static const zw_s2_tx_t ONCE[] = {ZW_S2_TX_JAMMED, ZW_S2_TX_UNKNOWN};
    hold = true;
    for (size_t i = 0; i < 3u; i++) {
        const size_t from = air_tail;
        TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_s2_send(&panel, now, DEV, ZW_SEC_AUTO, BASIC_GET,
                                                   sizeof BASIC_GET, 10u + i));
        TEST_ASSERT_EQUAL_UINT(1, air_tail - from);
        zw_s2_sent_tx(&panel, now, air[from % AIR].tag, AGAIN[i]);
        TEST_ASSERT_EQUAL_UINT(2, air_tail - from); /* on the air again */
        TEST_ASSERT_EQUAL_UINT(i, done_n[0]);
        hold = false;
        run_to(now + 1000u);
        hold = true;
        TEST_ASSERT_EQUAL_UINT(i + 1u, done_n[0]);
        TEST_ASSERT_EQUAL_INT(ZW_S2_OK, done[0][i].r);
    }
    for (size_t i = 0; i < 2u; i++) {
        const size_t from = air_tail;
        TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_s2_send(&panel, now, DEV, ZW_SEC_AUTO, BASIC_GET,
                                                   sizeof BASIC_GET, 20u + i));
        zw_s2_sent_tx(&panel, now, air[from % AIR].tag, ONCE[i]);
        TEST_ASSERT_EQUAL_UINT(1, air_tail - from);
        TEST_ASSERT_EQUAL_UINT(4u + i, done_n[0]);
        TEST_ASSERT_EQUAL_INT(ZW_S2_FAILED, done[0][3u + i].r);
        air_head = air_tail; /* the frame never went */
    }
    TEST_ASSERT_EQUAL_UINT(3, zw_s2_node_stats(&panel, DEV)->retries);
}

/* ZW-037: a sleeping device's NoAck means it is asleep: the frame goes once (its owner puts the
 * command back into the wake-up queue, network.md §7). */
static void test_sleeper_noack_not_sent_again(void)
{
    pair(ZW_S2_KEY_AUTH);
    zw_s2_set_attempts(&panel, panel_attempts);
    from_dev(ZW_SEC_AUTO, door_open, door_len);
    run_to(now + 1000u);
    dev_sleeps = true;
    const size_t from = air_tail;
    lose_panel = 1u;
    from_panel(ZW_SEC_AUTO, BASIC_GET, sizeof BASIC_GET, 5u);
    TEST_ASSERT_EQUAL_UINT(1, panel_frames_since(from));
    TEST_ASSERT_EQUAL_UINT(1, done_n[0]);
    TEST_ASSERT_EQUAL_INT(ZW_S2_FAILED, done[0][0].r);
    TEST_ASSERT_EQUAL_UINT(1, asked);
    TEST_ASSERT_EQUAL_UINT16(DEV, asked_node);
    TEST_ASSERT_EQUAL_UINT(0, zw_s2_node_stats(&panel, DEV)->retries);
}

/* ZW-007, ZW-037: a Nonce Report the device does not hear is not sent again (§6: one attempt,
 * whatever the owner's count) - the device asks again if it still needs one. */
static void test_nonce_report_not_sent_again(void)
{
    pair(ZW_S2_KEY_AUTH);
    zw_s2_set_attempts(&panel, panel_attempts);
    const size_t from = air_tail;
    lose_panel = 1u;
    uint8_t get[3];
    TEST_ASSERT_EQUAL_UINT(3, zw_s2_nonce_get_build(get, sizeof get, 0x21));
    zw_s2_receive(&panel, now, DEV, get, sizeof get);
    pump();
    TEST_ASSERT_EQUAL_UINT(1, panel_frames_since(from));
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_NONCE_REPORT, air[from % AIR].b[1]);
    TEST_ASSERT_EQUAL_UINT(0, asked);
    TEST_ASSERT_EQUAL_UINT(0, zw_s2_node_stats(&panel, DEV)->retries);
    TEST_ASSERT_EQUAL_UINT(1, zw_s2_node_stats(&panel, DEV)->nonce_reports);
    /* the next Nonce Get gets a new one */
    TEST_ASSERT_EQUAL_UINT(3, zw_s2_nonce_get_build(get, sizeof get, 0x22));
    zw_s2_receive(&panel, now, DEV, get, sizeof get);
    pump();
    TEST_ASSERT_EQUAL_UINT(2, panel_frames_since(from));
    TEST_ASSERT_EQUAL_UINT(2, zw_s2_node_stats(&panel, DEV)->nonce_reports);
}

/* ZW-037: a jammed Nonce Get (Fail with 0 ticks: the module never had the air) waits
 * ZW_S2_JAM_WAIT_MS and goes again byte for byte - the same sequence number, not a delivery
 * attempt - and its round trip is timed from that transmission: a Nonce Report that never comes
 * fails it NONCE_WAIT_MS after the resend, not after the first. The next send's jammed Nonce Get
 * goes again the same way and the send is delivered. */
static void test_jammed_nonce_get_sent_again_as_it_was(void)
{
    pair(ZW_S2_KEY_AUTH);
    zw_s2_set_jammed(&panel, panel_jammed);
    for (unsigned k = 0; k < 2u; k++) {
        hold = true;
        const size_t from = air_tail;
        TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_s2_send(&panel, now, DEV, ZW_SEC_AUTO, BASIC_GET,
                                                   sizeof BASIC_GET, 30u + k));
        TEST_ASSERT_EQUAL_UINT(1, air_tail - from);
        const air_t first = air[from % AIR];
        TEST_ASSERT_EQUAL_HEX8(ZW_S2_NONCE_GET, first.b[1]);
        zw_s2_sent_tx(&panel, now, first.tag, ZW_S2_TX_JAMMED);
        air_head = air_tail; /* the frame never went */
        TEST_ASSERT_EQUAL_UINT(k + 1u, jam_asked);
        TEST_ASSERT_EQUAL_UINT8(1u, jam_last);
        TEST_ASSERT_EQUAL_UINT(k, done_n[0]);
        const uint64_t t0 = now;
        run_to(t0 + ZW_S2_JAM_WAIT_MS - 1u);
        TEST_ASSERT_EQUAL_UINT(1, air_tail - from); /* not yet */
        run_to(t0 + ZW_S2_JAM_WAIT_MS);
        TEST_ASSERT_EQUAL_UINT(2, air_tail - from);
        const air_t *again = &air[(from + 1u) % AIR];
        TEST_ASSERT_EQUAL_UINT(first.len, again->len);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(first.b, again->b, first.len);
        hold = false;
        if (k == 0u) {
            lose_dev = 1u; /* its Nonce Report is lost */
            const uint64_t t1 = now;
            run_to(t1 + NONCE_WAIT_MS - 1u);
            TEST_ASSERT_EQUAL_UINT(0, done_n[0]);
            run_to(t1 + NONCE_WAIT_MS);
            TEST_ASSERT_EQUAL_UINT(1, done_n[0]);
            TEST_ASSERT_EQUAL_INT(ZW_S2_NO_NONCE, done[0][0].r);
        } else {
            run_to(now + 1000u);
            TEST_ASSERT_EQUAL_UINT(2, done_n[0]);
            TEST_ASSERT_EQUAL_UINT32(31u, done[0][1].tag);
            TEST_ASSERT_EQUAL_INT(ZW_S2_OK, done[0][1].r);
        }
    }
    expect_got(1, 0, ZW_SEC_S2_AUTH, BASIC_GET, sizeof BASIC_GET);
    TEST_ASSERT_EQUAL_UINT(1, got_n[1]);
    TEST_ASSERT_EQUAL_UINT(0, zw_s2_node_stats(&panel, DEV)->retries);
}

/* ZW-037, ZW-007: an encrypted frame waits out a jam; meanwhile the node lost the SPAN (it
 * restarted) and answers a frame on the old one with an SOS Nonce Report. At the deadline the
 * frame is not resent as kept (the node could not decrypt it) but built again on the nonce the
 * report brought - a SPAN extension - with its jam count kept: jammed once more, the owner is
 * told it is the second. Then it goes again as it was and is delivered. */
static void test_jammed_frame_built_again_after_sos(void)
{
    pair(ZW_S2_KEY_AUTH);
    zw_s2_set_jammed(&panel, panel_jammed);
    from_dev(ZW_SEC_AUTO, door_open, door_len);
    run_to(now + 1000u); /* a SPAN in use */
    hold = true;
    size_t from = air_tail;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_s2_send(&panel, now, DEV, ZW_SEC_AUTO, BASIC_GET,
                                               sizeof BASIC_GET, 40u));
    TEST_ASSERT_EQUAL_UINT(1, air_tail - from);
    const air_t kept = air[from % AIR];
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_MESSAGE_ENCAPSULATION, kept.b[1]);
    TEST_ASSERT_EQUAL_HEX8(0u, kept.b[3] & ZW_S2_ENCAP_EXT); /* on the SPAN in use */
    zw_s2_sent_tx(&panel, now, kept.tag, ZW_S2_TX_JAMMED);
    air_head = air_tail;
    const uint64_t t0 = now;
    /* during the wait: the node restarts, hears a frame on the old SPAN, answers SOS */
    dev_up(ZW_S2_KEY_AUTH);
    zw_s2_receive(&dev, now, PANEL, kept.b, kept.len);
    TEST_ASSERT_EQUAL_UINT(0, got_n[1]);
    TEST_ASSERT_EQUAL_UINT(1, air_tail - air_head);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_NONCE_REPORT, air[air_head % AIR].b[1]);
    hold = false;
    pump(); /* the SOS reaches the panel, its frame still waiting */
    TEST_ASSERT_EQUAL_UINT(0, done_n[0]);
    hold = true;
    from = air_tail;
    run_to(t0 + ZW_S2_JAM_WAIT_MS);
    TEST_ASSERT_EQUAL_UINT(1, air_tail - from);
    const air_t rebuilt = air[from % AIR];
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_MESSAGE_ENCAPSULATION, rebuilt.b[1]);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_ENCAP_EXT, rebuilt.b[3] & ZW_S2_ENCAP_EXT);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_EXT_SPAN, rebuilt.b[5] & ZW_S2_EXT_TYPE);
    /* jammed again: the count kept across the rebuild */
    zw_s2_sent_tx(&panel, now, rebuilt.tag, ZW_S2_TX_JAMMED);
    air_head = air_tail;
    TEST_ASSERT_EQUAL_UINT(2u, jam_asked);
    TEST_ASSERT_EQUAL_UINT8(2u, jam_last);
    run_to(now + ZW_S2_JAM_WAIT_MS);
    TEST_ASSERT_EQUAL_UINT(2, air_tail - from);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rebuilt.b, air[(from + 1u) % AIR].b, rebuilt.len);
    hold = false;
    run_to(now + 1000u);
    expect_got(1, 0, ZW_SEC_S2_AUTH, BASIC_GET, sizeof BASIC_GET);
    TEST_ASSERT_EQUAL_UINT(1, got_n[1]);
    TEST_ASSERT_EQUAL_UINT(1, done_n[0]);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, done[0][0].r);
}

/* ZW-037, ZW-007: an encrypted frame waits out a jam while the node's own frame arrives and
 * decrypts on the shared SPAN, which moves on past the kept frame's nonce. At the deadline the
 * frame is built again on the SPAN's next nonce, so the node decrypts it the first time: no SOS
 * Nonce Report, no resend (which would use up the frame's one resend). */
static void test_jammed_frame_built_again_after_nodes_frame(void)
{
    pair(ZW_S2_KEY_AUTH);
    zw_s2_set_jammed(&panel, panel_jammed);
    from_dev(ZW_SEC_AUTO, door_open, door_len);
    run_to(now + 1000u); /* a SPAN in use */
    hold = true;
    const size_t from = air_tail;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_s2_send(&panel, now, DEV, ZW_SEC_AUTO, BASIC_GET,
                                               sizeof BASIC_GET, 50u));
    const air_t kept = air[from % AIR];
    zw_s2_sent_tx(&panel, now, kept.tag, ZW_S2_TX_JAMMED);
    air_head = air_tail;
    const uint64_t t0 = now;
    hold = false;
    from_dev(ZW_SEC_AUTO, door_closed, door_len); /* heard during the wait */
    expect_got(0, 1, ZW_SEC_S2_AUTH, door_closed, door_len);
    run_to(t0 + ZW_S2_JAM_WAIT_MS + 1000u);
    expect_got(1, 0, ZW_SEC_S2_AUTH, BASIC_GET, sizeof BASIC_GET);
    TEST_ASSERT_EQUAL_UINT(1, done_n[0]);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, done[0][0].r);
    TEST_ASSERT_EQUAL_UINT(0, zw_s2_node_stats(&dev, PANEL)->decrypt_failures);
    TEST_ASSERT_EQUAL_UINT(0, zw_s2_node_stats(&panel, DEV)->resends);
}

/* ---- Scenarios from Z-Wave JS's test suite (hardening) ------------------------------------- */

/* ZW-009: an unencrypted frame posing as the continuation of a secure node's encrypted,
 * multi-report answer is dropped, and the encrypted answer still counts, whole. Mirrors Z-Wave
 * JS's partialCCSessions.test.ts, its last case ("Unencrypted reports cannot be injected into a
 * partial CC session at a higher security level"). The manager keeps no partial-report session:
 * each frame is judged on its own by §8, so a forged continuation has nothing to join. The sensor
 * answers the panel's Association Get with two reports - "one more follows", then the last - at its
 * class; between them a frame in the clear from its node ID claims to be the last, listing another
 * node. Both genuine parts reach the owner in order, at the sensor's class; the forgery is counted
 * at the wrong level and changes nothing else: no resync, the last part decrypts on the running
 * SPAN. */
static void test_unencrypted_continuation_dropped(void)
{
    pair(ZW_S2_KEY_UNAUTH);
    static const uint8_t ASSOCIATION_GET[] = {0x85, 0x02, 0x01};
    static const uint8_t THE_PANEL[] = {PANEL};
    static const uint8_t A_SIREN[] = {7u};
    static const uint8_t THE_THIEF[] = {9u};
    static const uint8_t ITS[] = {0x85}; /* it supports Association: asked encrypted */
    TEST_ASSERT_TRUE(zw_s2_set_secure_ccs(&panel, DEV, ITS, sizeof ITS));
    uint8_t part1[16], part2[16], forged[16];
    zw_cc_association_report_t r = {.group = 1u, .max_nodes = 5u, .reports_to_follow = 1u,
                                    .node_ids = THE_PANEL, .node_id_count = 1u};
    const size_t n1 = zw_cc_association_report_build(part1, sizeof part1, &r);
    r.reports_to_follow = 0u;
    r.node_ids = A_SIREN;
    const size_t n2 = zw_cc_association_report_build(part2, sizeof part2, &r);
    r.node_ids = THE_THIEF;
    const size_t nf = zw_cc_association_report_build(forged, sizeof forged, &r);
    TEST_ASSERT_TRUE(n1 > 0u && n2 > 0u && nf == n2);

    from_panel(ZW_SEC_AUTO, ASSOCIATION_GET, sizeof ASSOCIATION_GET, 1u);
    expect_got(1, 0, ZW_SEC_S2_UNAUTH, ASSOCIATION_GET, sizeof ASSOCIATION_GET);
    from_dev(ZW_SEC_AUTO, part1, n1);
    run_to(now + 100u);
    const uint32_t nonce_reports = zw_s2_node_stats(&panel, DEV)->nonce_reports;
    zw_s2_receive(&panel, now, DEV, forged, nf); /* in the clear, from the sensor's node ID */
    run_to(now + 500u); /* the first part's send is over on the sensor's side */
    from_dev(ZW_SEC_AUTO, part2, n2);

    TEST_ASSERT_EQUAL_UINT(2, got_n[0]);
    expect_got(0, 0, ZW_SEC_S2_UNAUTH, part1, n1);
    expect_got(0, 1, ZW_SEC_S2_UNAUTH, part2, n2);
    TEST_ASSERT_EQUAL_UINT(1, count_events(0, ZW_S2_EV_WRONG_LEVEL));
    TEST_ASSERT_EQUAL_UINT32(1, zw_s2_node_stats(&panel, DEV)->wrong_level);
    TEST_ASSERT_EQUAL_UINT32(0, zw_s2_node_stats(&panel, DEV)->decrypt_failures);
    TEST_ASSERT_EQUAL_UINT32(nonce_reports, zw_s2_node_stats(&panel, DEV)->nonce_reports);
    run_to(now + 1000u);
    TEST_ASSERT_EQUAL_UINT(2, done_n[1]); /* both parts delivered and not sent again */
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, done[1][0].r);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, done[1][1].r);
}

/* ZW-007, ZW-009, §8 "Sending", §9: a SPAN at a lower class never carries the panel's own
 * traffic. Mirrors Z-Wave JS's decodeLowerS2Keys.test.ts. The sensor holds Authenticated and
 * Unauthenticated (its highest: Authenticated) and speaks at Unauthenticated: a command of a secure
 * CC at that class decrypts - no Nonce Report for it - but is dropped and answered with nothing;
 * its Commands Supported Get at that class gets the empty list at that class, again without a
 * Nonce Report. A SPAN at Unauthenticated runs now. The panel's next command still goes at
 * Authenticated: a Nonce Get first, then the command on a new SPAN, with its extension. */
static void test_panel_sends_at_highest_despite_lower_span(void)
{
    pair(ZW_S2_KEY_AUTH | ZW_S2_KEY_UNAUTH);
    static const uint8_t TIME_GET[] = {0x8A, 0x01};
    static const uint8_t GET[] = {0x9F, 0x0D};
    static const uint8_t EMPTY[] = {0x9F, 0x0E};
    from_dev(ZW_SEC_S2_UNAUTH, TIME_GET, sizeof TIME_GET);
    TEST_ASSERT_EQUAL_UINT(0, got_n[0]);
    TEST_ASSERT_EQUAL_UINT(1, count_events(0, ZW_S2_EV_WRONG_LEVEL));
    TEST_ASSERT_EQUAL_UINT32(0, zw_s2_node_stats(&panel, DEV)->decrypt_failures);
    /* the one Nonce Report so far answered the sensor's own Nonce Get (it had no SPAN) */
    TEST_ASSERT_EQUAL_UINT32(1, zw_s2_node_stats(&panel, DEV)->nonce_reports);
    TEST_ASSERT_EQUAL_UINT(0, got_n[1]); /* and nothing answered the Time Get */
    run_to(now + 1000u);

    from_dev(ZW_SEC_S2_UNAUTH, GET, sizeof GET);
    expect_got(1, 0, ZW_SEC_S2_UNAUTH, EMPTY, sizeof EMPTY);
    TEST_ASSERT_EQUAL_UINT32(1, zw_s2_node_stats(&panel, DEV)->nonce_reports);
    TEST_ASSERT_EQUAL_INT(ZW_S2_SPAN_READY, zw_s2_span_state(&panel, DEV));
    TEST_ASSERT_EQUAL_UINT8(ZW_SEC_S2_UNAUTH, panel.peers[0].span_class);
    run_to(now + 1000u);

    const size_t before = heard_n;
    from_panel(ZW_SEC_AUTO, BASIC_GET, sizeof BASIC_GET, 5u);
    TEST_ASSERT_TRUE(heard_n >= before + 3u);
    TEST_ASSERT_EQUAL_UINT16(PANEL, heard[before].from);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_NONCE_GET, heard[before].b[1]);
    TEST_ASSERT_EQUAL_UINT16(DEV, heard[before + 1u].from);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_NONCE_REPORT, heard[before + 1u].b[1]);
    TEST_ASSERT_EQUAL_UINT16(PANEL, heard[before + 2u].from);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_MESSAGE_ENCAPSULATION, heard[before + 2u].b[1]);
    TEST_ASSERT_EQUAL_HEX8(ZW_S2_ENCAP_EXT, heard[before + 2u].b[3]); /* a new SPAN's extension */
    expect_got(1, 1, ZW_SEC_S2_AUTH, BASIC_GET, sizeof BASIC_GET);
    TEST_ASSERT_EQUAL_UINT8(ZW_SEC_S2_AUTH, panel.peers[0].span_class);
    run_to(now + 1000u);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, done[0][done_n[0] - 1u].r);
}

/* command-classes.md §5 (ZW-024): the panel's answers to a device's Get are replies it owes now -
 * a sleeping device that asked listens for them, so they never wait for its next wake-up (a Time
 * Report an hour late would be wrong); its own commands and reports are not. */
static void test_answers_are_replies(void)
{
    static const uint8_t owed[][3] = {{0x6C, 0x02, 0x01}, {0x8A, 0x02, 0x0E}, {0x8A, 0x04, 0x07},
                                      {0x86, 0x12, 0x03}, {0x86, 0x14, 0x5E}, {0x72, 0x05, 0xFF},
                                      {0x5E, 0x02, 0x02}, {0x85, 0x03, 0x01}, {0x85, 0x06, 0x01},
                                      {0x85, 0x0C, 0x00}, {0x8E, 0x03, 0x01}, {0x59, 0x04, 0x01}};
    for (size_t i = 0; i < sizeof owed / sizeof owed[0]; i++) {
        TEST_ASSERT_TRUE(zw_s2_command_is_reply(owed[i], 3u));
    }
    static const uint8_t not_owed[][3] = {{0x8A, 0x01, 0x00}, {0x20, 0x01, 0xFF}, {0x71, 0x05, 0x00},
                                          {0x86, 0x11, 0x00}, {0x6C, 0x01, 0x05}, {0x87, 0x01, 0x00}};
    for (size_t i = 0; i < sizeof not_owed / sizeof not_owed[0]; i++) {
        TEST_ASSERT_FALSE(zw_s2_command_is_reply(not_owed[i], 3u));
    }
    uint8_t mc[16];
    const size_t n = zw_cc_wrap_multi_channel(mc, sizeof mc, 0u, 2u, false, owed[1], 3u);
    TEST_ASSERT_TRUE(n > 0u && zw_s2_command_is_reply(mc, n)); /* to an end point too */
    TEST_ASSERT_FALSE(zw_s2_command_is_reply(owed[1], 1u));
    TEST_ASSERT_FALSE(zw_s2_command_is_reply(NULL, 3u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_codecs_round_trip);
    RUN_TEST(test_answers_are_replies);
    RUN_TEST(test_extension_rules);
    RUN_TEST(test_vector_frames_decrypt);
    RUN_TEST(test_multicast_follow_up_one_attempt);
    RUN_TEST(test_first_frame_establishes_span);
    RUN_TEST(test_span_frame_ahead_of_long_command);
    RUN_TEST(test_panel_restart_resync);
    RUN_TEST(test_lost_frames);
    RUN_TEST(test_crossed_frames);
    RUN_TEST(test_nodes_next_frame_ends_the_wait);
    RUN_TEST(test_duplicates_dropped);
    RUN_TEST(test_nonce_reports_without_floods);
    RUN_TEST(test_nonce_report_forgets_last_seq);
    RUN_TEST(test_device_restart_panel_resends);
    RUN_TEST(test_not_decoded_twice);
    RUN_TEST(test_no_nonce_report);
    RUN_TEST(test_sends_in_order);
    RUN_TEST(test_resend_new_nonce_and_sequence);
    RUN_TEST(test_three_noacks_fail);
    RUN_TEST(test_which_ends_are_sent_again);
    RUN_TEST(test_sleeper_noack_not_sent_again);
    RUN_TEST(test_nonce_report_not_sent_again);
    RUN_TEST(test_jammed_nonce_get_sent_again_as_it_was);
    RUN_TEST(test_jammed_frame_built_again_after_sos);
    RUN_TEST(test_jammed_frame_built_again_after_nodes_frame);
    RUN_TEST(test_send_choices_and_refusals);
    RUN_TEST(test_unencrypted_door_closed_dropped);
    RUN_TEST(test_clear_supervision_around_secure_dropped);
    RUN_TEST(test_lower_class_dropped);
    RUN_TEST(test_unknown_node_dropped);
    RUN_TEST(test_s2_inside_multichannel_dropped);
    RUN_TEST(test_repeated_layer_dropped);
    RUN_TEST(test_s2_supervision_multichannel_accepted);
    RUN_TEST(test_multi_command_crc_and_unsupported);
    RUN_TEST(test_commands_supported);
    RUN_TEST(test_unlisted_cc_below_class_dropped);
    RUN_TEST(test_stale_nif_report_dropped);
    RUN_TEST(test_crc16_around_s2);
    RUN_TEST(test_s0_node_rule);
    RUN_TEST(test_temporary_key);
    RUN_TEST(test_exchange_class_verify);
    RUN_TEST(test_no_key_no_nonce);
    RUN_TEST(test_failure_kinds);
    RUN_TEST(test_unencrypted_continuation_dropped);
    RUN_TEST(test_panel_sends_at_highest_despite_lower_span);
    RUN_TEST(test_nif_adds_never_removes);
    RUN_TEST(test_s0_nonce_and_scheme_in_clear_from_s2_node);
    RUN_TEST(test_group_get_and_multi_command);
    RUN_TEST(test_gets_table_whole);
    RUN_TEST(test_commands_supported_get_heard);
    RUN_TEST(test_unknown_classes_learnt_by_decrypting);
    RUN_TEST(test_unknown_classes_without_keys);
    RUN_TEST(test_unknown_classes_learnt_class_does_not_lock);
    return UNITY_END();
}
