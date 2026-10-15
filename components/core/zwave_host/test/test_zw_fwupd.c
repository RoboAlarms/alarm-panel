/* ZW-033 (docs/features/36): a device's firmware sent over Firmware Update Meta Data - against a
 * scripted device: the whole transfer, each refusal and failure, activation, time-outs, failed
 * sends, fragments asked again, an abort. */
#include <string.h>

#include "unity.h"
#include "zwave_host/zw_fwupd.h"
#include "zwave_proto/zw_crc16.h"

#define NODE 7u
#define MANUF 0x015Au

static zw_fwupd_t F;
static uint8_t IMAGE[100];
static struct {
    uint8_t cmd[80];
    size_t len;
    uint32_t tag;
} SENT[64];
static size_t N;
static bool QUEUE_FAILS;
static uint64_t NOW;

static bool port_send(void *ctx, uint16_t node, const uint8_t *cmd, size_t len, uint32_t tag)
{
    (void)ctx;
    TEST_ASSERT_EQUAL_UINT16(NODE, node);
    if (QUEUE_FAILS) {
        return false;
    }
    TEST_ASSERT_TRUE(N < 64u && len <= sizeof SENT[0].cmd);
    memcpy(SENT[N].cmd, cmd, len);
    SENT[N].len = len;
    SENT[N].tag = tag;
    N++;
    return true;
}

static const uint8_t *last(void) { return SENT[N - 1u].cmd; }

static void done(bool delivered) { (void)zw_fwupd_send_done(&F, NOW, SENT[N - 1u].tag, delivered); }

static void rx(const uint8_t *cmd, size_t len) { TEST_ASSERT_TRUE(zw_fwupd_received(&F, NOW, NODE, cmd, len)); }

static void md(uint16_t manuf, bool upgradable, uint16_t max_frag, bool activation)
{
    zw_fwu_md_t m = {.manufacturer_id = manuf, .firmware_id = 0x0003, .checksum = 0x1111,
                     .upgradable = upgradable, .max_fragment = max_frag, .hardware = 5,
                     .supports_activation = activation};
    uint8_t out[32];
    const size_t n = zw_fwu_md_report_build(out, sizeof out, &m);
    rx(out, n);
}

void setUp(void)
{
    for (size_t i = 0; i < sizeof IMAGE; i++) {
        IMAGE[i] = (uint8_t)(i * 7u + 3u);
    }
    N = 0;
    QUEUE_FAILS = false;
    NOW = 1000u;
    const zw_fwupd_port_t port = {NULL, port_send};
    zw_fwupd_init(&F, &port);
}
void tearDown(void) {}

static void start(uint8_t cc_version)
{
    TEST_ASSERT_TRUE(zw_fwupd_start(&F, NOW, NODE, cc_version, MANUF, 0, IMAGE, sizeof IMAGE, 46));
    TEST_ASSERT_EQUAL_HEX8(ZW_FWU_MD_GET, last()[1]);
    done(true);
}

static void get(uint8_t reports, uint16_t number)
{
    const zw_fwu_get_t g = {reports, number};
    uint8_t out[8];
    rx(out, zw_fwu_get_build(out, sizeof out, &g));
}

/* The fragments sent put together again, each one's CRC checked. */
static void expect_image_from(size_t first, size_t count, uint16_t fragment)
{
    for (size_t i = 0; i < count; i++) {
        uint16_t number = 0;
        bool is_last = false;
        const uint8_t *d = NULL;
        size_t dn = 0;
        TEST_ASSERT_TRUE(zw_fwu_report_parse(SENT[first + i].cmd, SENT[first + i].len, true,
                                             &number, &is_last, &d, &dn));
        const size_t at = (size_t)(number - 1u) * fragment;
        TEST_ASSERT_EQUAL_MEMORY(&IMAGE[at], d, dn);
    }
}

static void test_whole_transfer(void)
{
    start(5);
    md(MANUF, true, 20, false);
    /* Request Get: its checksum, target 0, the device's 20-byte fragments, its hardware */
    const uint8_t *q = last();
    TEST_ASSERT_EQUAL_HEX8(ZW_FWU_REQUEST_GET, q[1]);
    zw_fwu_request_t r;
    TEST_ASSERT_TRUE(zw_fwu_request_get_parse(q, SENT[N - 1u].len, &r));
    TEST_ASSERT_EQUAL_HEX16(zw_crc16(ZW_CRC16_INIT, IMAGE, sizeof IMAGE), r.checksum);
    TEST_ASSERT_EQUAL_UINT16(20, r.fragment_size);
    TEST_ASSERT_EQUAL_UINT8(5, r.hardware);
    TEST_ASSERT_FALSE(r.activation);
    done(true);
    const uint8_t ok[] = {0x7A, 0x04, 0xFF};
    rx(ok, sizeof ok);
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_TRANSFER, F.state);
    const size_t first = N;
    get(2, 1);
    TEST_ASSERT_EQUAL_UINT(first + 1u, N); /* one at a time */
    done(true);
    TEST_ASSERT_EQUAL_UINT(first + 2u, N);
    done(true);
    TEST_ASSERT_EQUAL_UINT(first + 2u, N); /* the Get's two, no more */
    get(5, 3); /* 100 bytes / 20: 5 fragments; it asks for more than there are */
    for (unsigned i = 0; i < 3u; i++) {
        done(true);
    }
    TEST_ASSERT_EQUAL_UINT(first + 5u, N);
    TEST_ASSERT_EQUAL_HEX8(0x80, SENT[N - 1u].cmd[2] & 0x80u); /* the last one flagged */
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_STATUS, F.state);
    TEST_ASSERT_EQUAL_UINT16(5, F.sent_max);
    expect_image_from(first, 5, 20);
    const uint8_t st[] = {0x7A, 0x07, 0xFF, 0x00, 0x05};
    rx(st, sizeof st);
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_OK_RESTART, F.result);
    TEST_ASSERT_EQUAL_UINT16(5, F.wait_s);
    TEST_ASSERT_FALSE(zw_fwupd_running(&F));
}

static void test_refusals(void)
{
    start(5);
    md(0x0086, true, 20, false); /* another maker than the node cache knows */
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_WRONG_DEVICE, F.result);
    start(5);
    md(MANUF, false, 20, false);
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_NOT_UPGRADABLE, F.result);
    start(5);
    md(MANUF, true, 20, false);
    done(true);
    const uint8_t low[] = {0x7A, 0x04, 0x06};
    rx(low, sizeof low);
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_REFUSED, F.result);
    TEST_ASSERT_EQUAL_HEX8(ZW_FWU_REQ_BATTERY_LOW, F.detail);
    /* its verdict: a checksum error */
    start(5);
    md(MANUF, true, 50, false);
    done(true);
    const uint8_t ok[] = {0x7A, 0x04, 0xFF};
    rx(ok, sizeof ok);
    get(2, 1);
    done(true);
    done(true);
    const uint8_t bad[] = {0x7A, 0x07, 0x00};
    rx(bad, sizeof bad);
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_DEVICE_FAILED, F.result);
    TEST_ASSERT_EQUAL_HEX8(ZW_FWU_STATUS_CHECKSUM, F.detail);
    /* another node's or another CC's commands are not the update's */
    start(5);
    const uint8_t other[] = {0x71, 0x05, 0};
    TEST_ASSERT_FALSE(zw_fwupd_received(&F, NOW, NODE, other, sizeof other));
    TEST_ASSERT_FALSE(zw_fwupd_received(&F, NOW, NODE + 1u, low, sizeof low));
    TEST_ASSERT_FALSE(zw_fwupd_start(&F, NOW, NODE, 5, MANUF, 0, IMAGE, sizeof IMAGE, 46)); /* busy */
}

static void test_activation(void)
{
    start(7);
    md(MANUF, true, 50, true);
    zw_fwu_request_t r;
    TEST_ASSERT_TRUE(zw_fwu_request_get_parse(last(), SENT[N - 1u].len, &r));
    TEST_ASSERT_TRUE(r.activation);
    done(true);
    const uint8_t ok[] = {0x7A, 0x04, 0xFF};
    rx(ok, sizeof ok);
    get(2, 1);
    done(true);
    done(true);
    const uint8_t wait[] = {0x7A, 0x07, 0xFD};
    rx(wait, sizeof wait);
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_ACTIVATE, F.state);
    TEST_ASSERT_EQUAL_HEX8(ZW_FWU_ACTIVATION_SET, last()[1]);
    done(true);
    uint8_t rep[] = {0x7A, 0x09, 0x01, 0x5A, 0, 3, 0, 0, 0, 0xFF};
    rx(rep, sizeof rep);
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_OK_RESTART, F.result);
    /* activation failing */
    start(7);
    md(MANUF, true, 50, true);
    done(true);
    rx(ok, sizeof ok);
    get(2, 1);
    done(true);
    done(true);
    rx(wait, sizeof wait);
    done(true);
    rep[9] = 0x01;
    rx(rep, sizeof rep);
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_ACTIVATION_FAILED, F.result);
}

/* Each wait ends: the meta data asked 3 times, then TIMEOUT; a device that stops asking for
 * fragments; one whose verdict never comes. */
static void test_timeouts(void)
{
    start(5);
    for (unsigned i = 0; i < 2u; i++) {
        NOW += ZW_FWUPD_MD_MS;
        zw_fwupd_poll(&F, NOW);
        TEST_ASSERT_EQUAL_HEX8(ZW_FWU_MD_GET, last()[1]);
        done(true);
    }
    NOW += ZW_FWUPD_MD_MS;
    zw_fwupd_poll(&F, NOW);
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_TIMEOUT, F.result);
    start(5);
    md(MANUF, true, 50, false);
    done(true);
    const uint8_t ok[] = {0x7A, 0x04, 0xFF};
    rx(ok, sizeof ok);
    get(1, 1);
    done(true);
    NOW += ZW_FWUPD_GET_MS - 1u;
    zw_fwupd_poll(&F, NOW);
    TEST_ASSERT_TRUE(zw_fwupd_running(&F));
    NOW += 1u;
    zw_fwupd_poll(&F, NOW);
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_TIMEOUT, F.result);
    TEST_ASSERT_EQUAL_UINT8(ZW_FWUPD_TRANSFER, F.detail);
    start(5);
    md(MANUF, true, 50, false);
    done(true);
    rx(ok, sizeof ok);
    get(3, 1); /* 100 bytes in 40-byte fragments (46 - 6): 3 */
    done(true);
    done(true);
    done(true);
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_STATUS, F.state);
    NOW += ZW_FWUPD_STATUS_MS;
    zw_fwupd_poll(&F, NOW);
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_TIMEOUT, F.result);
    TEST_ASSERT_EQUAL_UINT8(ZW_FWUPD_STATUS, F.detail);
}

/* ZW-033: a sleeping device - its MD Get waits for its wake-up, never asked again meanwhile; once
 * it went, the usual waits; one that never wakes in the allowed time ends TIMEOUT in state MD. */
static void test_sleeping_device(void)
{
    zw_fwupd_expect_sleep(&F, 3600000u);
    TEST_ASSERT_TRUE(zw_fwupd_start(&F, NOW, NODE, 5, MANUF, 0, IMAGE, sizeof IMAGE, 46));
    TEST_ASSERT_TRUE(F.waiting_wake);
    const size_t sent = N;
    NOW += 30u * 60000u; /* half an hour asleep: nothing asked again, nothing ended */
    zw_fwupd_poll(&F, NOW);
    TEST_ASSERT_EQUAL_UINT(sent, N);
    TEST_ASSERT_TRUE(zw_fwupd_running(&F));
    done(true); /* it woke: the MD Get went */
    TEST_ASSERT_FALSE(F.waiting_wake);
    TEST_ASSERT_EQUAL_UINT64(NOW + ZW_FWUPD_MD_MS, zw_fwupd_next_deadline(&F));
    md(MANUF, true, 50, false);
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_REQUEST, F.state);

    /* the next start is an ordinary one again; one that never wakes */
    zw_fwupd_abort(&F, NOW);
    zw_fwupd_expect_sleep(&F, 60000u);
    TEST_ASSERT_TRUE(zw_fwupd_start(&F, NOW, NODE, 5, MANUF, 0, IMAGE, sizeof IMAGE, 46));
    NOW += 59999u;
    zw_fwupd_poll(&F, NOW);
    TEST_ASSERT_TRUE(zw_fwupd_running(&F));
    NOW += 1u;
    zw_fwupd_poll(&F, NOW);
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_TIMEOUT, F.result);
    TEST_ASSERT_EQUAL_UINT8(ZW_FWUPD_MD, F.detail);
    TEST_ASSERT_FALSE(F.waiting_wake);
    start(5);
    TEST_ASSERT_FALSE(F.waiting_wake);
}

/* ZW-033: the device's next Get arrives before the last fragment's send has ended (its radio
 * answers faster than the module reports): that Get's fragments follow in order - none skipped,
 * and one it asks for again while it is on its way is sent again. (Both found with the simulated
 * device: fragment 3 was skipped; a lost fragment 5 was never sent again.) */
static void test_get_before_send_end(void)
{
    start(5);
    md(MANUF, true, 20, false); /* 20-byte fragments (26 - 6): 100 bytes in 5 */
    done(true);
    const uint8_t ok[] = {0x7A, 0x04, 0xFF};
    rx(ok, sizeof ok);
    get(2, 1);
    done(true);                 /* fragment 1 */
    TEST_ASSERT_EQUAL_UINT16(2u, (uint16_t)(last()[2] << 8 | last()[3]));
    get(2, 3);                  /* fragment 2 is still on its way when the Get for 3-4 comes */
    const size_t before = N;
    done(true);                 /* fragment 2's end */
    TEST_ASSERT_EQUAL_UINT(before + 1u, N);
    TEST_ASSERT_EQUAL_UINT16(3u, (uint16_t)(last()[2] << 8 | last()[3]));
    TEST_ASSERT_EQUAL_UINT16(2u, F.sent_max);
    done(true);
    TEST_ASSERT_EQUAL_UINT16(4u, (uint16_t)(last()[2] << 8 | last()[3]));
    done(true);
    TEST_ASSERT_EQUAL_UINT16(4u, F.sent_max);
    get(1, 5);
    TEST_ASSERT_EQUAL_UINT16(5u, (uint16_t)(last()[2] << 8 | last()[3]) & 0x7FFFu);
    TEST_ASSERT_TRUE((last()[2] & 0x80u) != 0u); /* the last */
    /* the device lost it and asks for it again before its send has ended: sent again */
    get(1, 5);
    const size_t again = N;
    done(true);
    TEST_ASSERT_EQUAL_UINT(again + 1u, N);
    TEST_ASSERT_EQUAL_UINT16(5u, (uint16_t)(last()[2] << 8 | last()[3]) & 0x7FFFu);
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_TRANSFER, F.state);
    done(true);
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_STATUS, F.state);
}

/* ZW-033: the Request Get's own end comes after the device's Request Report and its first Get
 * (a fast device, a slow module): it is no fragment - nothing counted as sent, nothing skipped;
 * after an earlier update too (whose last fragment number must not come back). */
static void test_request_end_after_first_get(void)
{
    for (unsigned round = 0; round < 2u; round++) {
        start(5);
        md(MANUF, true, 20, false);
        const uint8_t ok[] = {0x7A, 0x04, 0xFF};
        rx(ok, sizeof ok);     /* before the Request Get's end */
        get(2, 1);
        TEST_ASSERT_EQUAL_UINT16(0u, F.sent_max);
        const size_t before = N;
        done(true);            /* the Request Get's end: nothing moves */
        TEST_ASSERT_EQUAL_UINT(before + 1u, N);
        TEST_ASSERT_EQUAL_UINT16(1u, (uint16_t)(last()[2] << 8 | last()[3]));
        TEST_ASSERT_EQUAL_UINT16(0u, F.sent_max);
        TEST_ASSERT_EQUAL_INT(ZW_FWUPD_TRANSFER, F.state);
        for (uint16_t n = 1u; n <= 5u; n++) {
            if (n % 2u == 1u && n > 1u) {
                get(2, n);
            }
            done(true);
        }
        TEST_ASSERT_EQUAL_UINT16(5u, F.sent_max);
        TEST_ASSERT_EQUAL_INT(ZW_FWUPD_STATUS, F.state);
        const uint8_t verdict[] = {0x7A, 0x07, 0xFF, 0x00, 0x00};
        rx(verdict, sizeof verdict);
        TEST_ASSERT_EQUAL_INT(ZW_FWUPD_OK_RESTART, F.result);
    }
}

/* A fragment not delivered is sent again (the same one); three in a row fail it. The device may
 * ask for a fragment again, earlier ones included. An abort ends it, and a late send's end is
 * ignored. */
static void test_failed_sends_rerequests_and_abort(void)
{
    start(5);
    md(MANUF, true, 20, false);
    done(true);
    const uint8_t ok[] = {0x7A, 0x04, 0xFF};
    rx(ok, sizeof ok);
    get(1, 1);
    const size_t first = N;
    done(false);
    TEST_ASSERT_EQUAL_UINT(first + 1u, N);
    TEST_ASSERT_EQUAL_MEMORY(SENT[first - 1u].cmd, SENT[first].cmd, SENT[first].len); /* again */
    done(true);
    get(1, 2);
    done(true);
    get(1, 1); /* it asks for #1 again */
    uint16_t number = 0;
    bool is_last = false;
    const uint8_t *d = NULL;
    size_t dn = 0;
    TEST_ASSERT_TRUE(zw_fwu_report_parse(last(), SENT[N - 1u].len, true, &number, &is_last, &d, &dn));
    TEST_ASSERT_EQUAL_UINT16(1, number);
    done(false);
    done(false);
    done(false);
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_SEND_FAILED, F.result);
    /* queued nothing at all: it tries again from the poll */
    start(5);
    md(MANUF, true, 20, false);
    done(true);
    rx(ok, sizeof ok);
    QUEUE_FAILS = true;
    get(1, 1);
    QUEUE_FAILS = false;
    const size_t before = N;
    zw_fwupd_poll(&F, NOW);
    TEST_ASSERT_EQUAL_UINT(before + 1u, N);
    zw_fwupd_abort(&F, NOW);
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_ABORTED, F.result);
    TEST_ASSERT_TRUE(zw_fwupd_send_done(&F, NOW, SENT[N - 1u].tag, true)); /* its tag, ignored */
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_ABORTED, F.result);
    TEST_ASSERT_FALSE(zw_fwupd_send_done(&F, NOW, 0x12345678u, true));
}

/* A v1 device: no CRC on fragments, no target, its fragments sized from the frame alone. */
static void test_version_one_device(void)
{
    TEST_ASSERT_TRUE(zw_fwupd_start(&F, NOW, NODE, 1, MANUF, 0, IMAGE, sizeof IMAGE, 46));
    done(true);
    const uint8_t v1[] = {0x7A, 0x02, 0x01, 0x5A, 0x00, 0x03, 0x11, 0x11};
    rx(v1, sizeof v1);
    TEST_ASSERT_EQUAL_UINT(2u + 6u, SENT[N - 1u].len);
    done(true);
    const uint8_t ok[] = {0x7A, 0x04, 0xFF};
    rx(ok, sizeof ok);
    get(1, 1);
    TEST_ASSERT_EQUAL_UINT(4u + 42u, SENT[N - 1u].len); /* 46 - 4, no CRC */
}

/* A long transfer: 16000 bytes in 40-byte fragments (46 - 6), 400 of them. */
static uint8_t BIG[16000];

static void start_big(void)
{
    for (size_t i = 0; i < sizeof BIG; i++) {
        BIG[i] = (uint8_t)(i * 13u + 1u);
    }
    TEST_ASSERT_TRUE(zw_fwupd_start(&F, NOW, NODE, 5, MANUF, 0, BIG, sizeof BIG, 46));
    done(true);
    md(MANUF, true, 0, false);
    done(true);
    const uint8_t ok[] = {0x7A, 0x04, 0xFF};
    rx(ok, sizeof ok);
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_TRANSFER, F.state);
    TEST_ASSERT_EQUAL_UINT16(400, F.total);
}

/* The fragment on its way delivered `ms` later; SENT keeps only the send that follows it (a
 * long transfer has more sends than SENT holds). */
static void deliver_after(uint64_t ms)
{
    NOW += ms;
    zw_fwupd_poll(&F, NOW);
    TEST_ASSERT_TRUE(zw_fwupd_running(&F));
    const uint32_t tag = SENT[N - 1u].tag;
    N = 0;
    TEST_ASSERT_TRUE(zw_fwupd_send_done(&F, NOW, tag, true));
}

/* ZW-033 (code review ZW11): one Get asking for 200 fragments, each delivered 500 ms after the
 * one before - 100 s of steady progress - is not a device gone silent: the next-Get wait runs
 * from the last fragment delivered, so the batch completes, the next one too, and the last
 * fragment leads to the Status wait. Silence after a batch still ends it 60 s later. */
static void test_long_batch_completes(void)
{
    start_big();
    get(200, 1);
    for (unsigned i = 0; i < 200u; i++) {
        deliver_after(500u);
    }
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_TRANSFER, F.state);
    TEST_ASSERT_EQUAL_UINT16(200, F.sent_max);
    TEST_ASSERT_EQUAL_UINT(0u, N); /* the batch's 200, no more */
    zw_fwupd_poll(&F, NOW + ZW_FWUPD_GET_MS - 1u); /* the next Get awaited 60 s from here */
    TEST_ASSERT_TRUE(zw_fwupd_running(&F));
    NOW += 1000u;
    get(200, 201);
    for (unsigned i = 0; i < 199u; i++) {
        deliver_after(500u);
    }
    NOW += 500u;
    done(true); /* fragment 400, the last */
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_STATUS, F.state);
    TEST_ASSERT_EQUAL_UINT16(400, F.sent_max);
    TEST_ASSERT_EQUAL_UINT64(NOW + ZW_FWUPD_STATUS_MS, zw_fwupd_next_deadline(&F));
    const uint8_t st[] = {0x7A, 0x07, 0xFE}; /* OK, no restart */
    rx(st, sizeof st);
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_OK_NO_RESTART, F.result);
    /* silence after a batch: TIMEOUT 60 s after its last fragment, not before */
    start_big();
    get(150, 1);
    for (unsigned i = 0; i < 150u; i++) {
        deliver_after(500u);
    }
    zw_fwupd_poll(&F, NOW + ZW_FWUPD_GET_MS - 1u);
    TEST_ASSERT_TRUE(zw_fwupd_running(&F));
    zw_fwupd_poll(&F, NOW + ZW_FWUPD_GET_MS);
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_TIMEOUT, F.result);
    TEST_ASSERT_EQUAL_UINT8(ZW_FWUPD_TRANSFER, F.detail);
}

/* ZW-033 (code review ZW11): a batch that stalls - a fragment's send never ends - still ends
 * as TIMEOUT, 60 s after the last fragment delivered. */
static void test_stalled_batch_ends(void)
{
    start_big();
    get(200, 1);
    for (unsigned i = 0; i < 10u; i++) {
        deliver_after(500u);
    }
    const uint64_t progress = NOW; /* fragment 11 on its way, its end never comes */
    zw_fwupd_poll(&F, progress + ZW_FWUPD_GET_MS - 1u);
    TEST_ASSERT_TRUE(zw_fwupd_running(&F));
    TEST_ASSERT_EQUAL_UINT(1u, N); /* nothing sent beside it */
    zw_fwupd_poll(&F, progress + ZW_FWUPD_GET_MS);
    TEST_ASSERT_EQUAL_INT(ZW_FWUPD_TIMEOUT, F.result);
    TEST_ASSERT_EQUAL_UINT8(ZW_FWUPD_TRANSFER, F.detail);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_whole_transfer);
    RUN_TEST(test_refusals);
    RUN_TEST(test_activation);
    RUN_TEST(test_timeouts);
    RUN_TEST(test_failed_sends_rerequests_and_abort);
    RUN_TEST(test_version_one_device);
    RUN_TEST(test_sleeping_device);
    RUN_TEST(test_get_before_send_end);
    RUN_TEST(test_request_end_after_first_get);
    RUN_TEST(test_long_batch_completes);
    RUN_TEST(test_stalled_batch_ends);
    return UNITY_END();
}
