/*
 * test_zw_otw.c - the Z-Wave radio's own firmware, over the wire (docs/features/36 serial-api.md
 * §8, network.md §14; ZW-025; as Z-Wave JS's firmwareUpdateOTW): the host against the fake ZAC93's
 * Gecko bootloader - EnterBootloader, the menu's "upload gbl", XMODEM-CRC blocks, EOT, "run", the
 * start-up again - and every way it can end: a block sent again, a block never answered, the
 * bootloader refusing the file (an error 0x2X tried again), no upload in its menu, firmware that
 * doesn't start, and an update tried again from the bootloader.
 */
#include <string.h>

#include "fake_zac93.h"
#include "unity.h"
#include "zwave_host/zw_host.h"
#include "zwave_proto/zw_gbl.h"

static zw_host_t host;
static fake_zac93_t zac;
static fake_wire_t wire;

static struct {
    unsigned ups, downs, otw_done;
    zw_link_down_t down;
    zw_module_facts_t facts;
} heard;

static void on_link_up(void *ctx, const zw_module_facts_t *facts)
{
    (void)ctx;
    heard.ups++;
    heard.facts = *facts;
}

static void on_link_down(void *ctx, const zw_link_down_t *why)
{
    (void)ctx;
    heard.downs++;
    heard.down = *why;
}

static void on_otw_done(void *ctx)
{
    (void)ctx;
    heard.otw_done++;
}

static bool port_write(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    return fake_wire_write(&wire, bytes, len);
}

static void host_control(void *ctx, uint64_t now, uint8_t b) { zw_host_on_control(ctx, now, b); }
static void host_frame(void *ctx, uint64_t now, const zw_frame_t *f) { zw_host_on_frame(ctx, now, f); }
static void host_text(void *ctx, uint64_t now, const uint8_t *b, size_t n) { zw_host_on_text(ctx, now, b, n); }
static void host_poll(void *ctx, uint64_t now) { zw_host_poll(ctx, now); }
static uint64_t host_deadline(void *ctx) { return zw_host_next_deadline(ctx); }

static void run_for(uint64_t ms)
{
    fake_wire_run_until(&wire, wire.now + ms);
}

void setUp(void)
{
    const fake_host_t fh = {&host, host_control, host_frame, host_text, host_poll, host_deadline};
    memset(&heard, 0, sizeof heard);
    fake_zac93_init(&zac);
    fake_wire_init(&wire, &zac, &fh, 0u);
    const zw_host_port_t port = {.write = port_write, .link_up = on_link_up,
                                 .link_down = on_link_down, .otw_done = on_otw_done};
    zw_host_init(&host, &port, NULL);
    zw_host_start(&host, wire.now);
    run_for(3000u);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(&host));
    zac.next_firmware[0] = 2u;
    zac.next_firmware[1] = 30u;
}
void tearDown(void) {}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8 & 0xFFu);
    p[2] = (uint8_t)(v >> 16 & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

/* A GBL of `data_len` program bytes (the bootloader checks nothing of it here: the host did). */
static uint8_t image[4096];
static size_t make_gbl(size_t data_len)
{
    size_t at = 0u;
    put_u32(&image[at], ZW_GBL_TAG_HEADER);
    put_u32(&image[at + 4u], 8u);
    put_u32(&image[at + 8u], 0x03000000u);
    put_u32(&image[at + 12u], 0u);
    at += 16u;
    put_u32(&image[at], 0xF40A0AF4u);
    put_u32(&image[at + 4u], (uint32_t)(4u + data_len));
    put_u32(&image[at + 8u], 0u);
    for (size_t i = 0; i < data_len; i++) {
        image[at + 12u + i] = (uint8_t)(i * 13u + 5u);
    }
    at += 12u + data_len;
    put_u32(&image[at], ZW_GBL_TAG_END);
    put_u32(&image[at + 4u], 4u);
    put_u32(&image[at + 8u], zw_crc32(0u, image, at + 8u));
    return at + 12u;
}

/* The CRC-32 of the file as the bootloader receives it: padded with FF to whole blocks. */
static uint32_t padded_crc(size_t len)
{
    static uint8_t padded[sizeof image + 128u];
    const size_t blocks = zw_xmodem_blocks(len);
    memset(padded, 0xFF, blocks * 128u);
    memcpy(padded, image, len);
    return zw_crc32(0u, padded, blocks * 128u);
}

static const zw_otw_progress_t *until_done(uint64_t ms)
{
    for (uint64_t t = 0; t < ms && zw_host_otw(&host)->state != ZW_OTW_DONE; t += 100u) {
        run_for(100u);
    }
    return zw_host_otw(&host);
}

/* ZW-025: the whole file, block by block; the new firmware starts, the link comes back with it */
static void test_update(void)
{
    const size_t n = make_gbl(1000u);
    TEST_ASSERT_EQUAL_INT(ZW_OTW_BEGUN, zw_host_otw_begin(&host, wire.now, image, n));
    TEST_ASSERT_EQUAL_INT(ZW_OTW_BUSY, zw_host_otw_begin(&host, wire.now, image, n));
    TEST_ASSERT_EQUAL_INT(ZW_LINK_DOWN, zw_host_link(&host));
    TEST_ASSERT_EQUAL_INT(ZW_DOWN_UPDATING, heard.down.reason);
    zw_request_t req;
    memset(&req, 0, sizeof req);
    req.func = (uint8_t)ZW_FUNC_GET_CONTROLLER_ID;
    req.expect = ZW_EXPECT_RESPONSE;
    req.tag = 1u;
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_LINK_DOWN, zw_host_submit(&host, &req)); /* nothing else goes */
    const zw_otw_progress_t *p = until_done(60000u);
    TEST_ASSERT_EQUAL_INT(ZW_OTW_DONE, p->state);
    TEST_ASSERT_EQUAL_INT(ZW_OTW_OK, p->result);
    TEST_ASSERT_EQUAL_UINT32(zw_xmodem_blocks(n), p->total);
    TEST_ASSERT_EQUAL_UINT32(p->total, p->sent);
    TEST_ASSERT_EQUAL_UINT32(0u, p->resent);
    TEST_ASSERT_EQUAL_UINT32(1u, zac.uploads);
    TEST_ASSERT_EQUAL_HEX32(padded_crc(n), zac.xm_crc);
    TEST_ASSERT_EQUAL_UINT(1u, heard.otw_done);
    run_for(5000u);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(&host));
    TEST_ASSERT_EQUAL_UINT(2u, heard.ups);
    TEST_ASSERT_EQUAL_UINT8(2u, heard.facts.firmware_major); /* its new version */
    TEST_ASSERT_EQUAL_UINT8(30u, heard.facts.firmware_minor);
}

/* ZW-025: a damaged or foreign file touches nothing */
static void test_bad_file(void)
{
    const size_t n = make_gbl(300u);
    image[40] ^= 0x01u;
    TEST_ASSERT_EQUAL_INT(ZW_OTW_BAD_FILE, zw_host_otw_begin(&host, wire.now, image, n));
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(&host));
    TEST_ASSERT_FALSE(zac.fault.bootloader);
    TEST_ASSERT_EQUAL_UINT(0u, heard.downs);
}

/* ZW-025: a block NAKed twice is sent again; one never answered ends it after 3 sends, and the
 * old firmware comes back */
static void test_blocks_again_and_given_up(void)
{
    size_t n = make_gbl(1000u);
    zac.fault.otw_nak_block = 3u;
    zac.fault.otw_nak_times = 2u;
    TEST_ASSERT_EQUAL_INT(ZW_OTW_BEGUN, zw_host_otw_begin(&host, wire.now, image, n));
    const zw_otw_progress_t *p = until_done(60000u);
    TEST_ASSERT_EQUAL_INT(ZW_OTW_OK, p->result);
    TEST_ASSERT_EQUAL_UINT32(2u, p->resent);
    run_for(5000u);

    setUp();
    n = make_gbl(1000u);
    zac.fault.otw_silent_block = 4u;
    TEST_ASSERT_EQUAL_INT(ZW_OTW_BEGUN, zw_host_otw_begin(&host, wire.now, image, n));
    p = until_done(60000u);
    TEST_ASSERT_EQUAL_INT(ZW_OTW_RETRIES, p->result);
    TEST_ASSERT_EQUAL_UINT32(3u, p->sent);
    TEST_ASSERT_EQUAL_UINT32(0u, zac.uploads);
    run_for(10000u);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(&host)); /* "run": its old firmware */
    TEST_ASSERT_EQUAL_UINT8(1u, heard.facts.firmware_major);
}

/* ZW-025 (§8): the bootloader refuses the file - an error 0x2X is tried again (3 uploads in all),
 * another ends it with its code */
static void test_bootloader_refuses(void)
{
    size_t n = make_gbl(700u);
    zac.fault.otw_cancel_block = 2u;
    zac.fault.otw_cancel_error = 0x21u;
    zac.fault.otw_cancel_times = 1u;
    TEST_ASSERT_EQUAL_INT(ZW_OTW_BEGUN, zw_host_otw_begin(&host, wire.now, image, n));
    const zw_otw_progress_t *p = until_done(60000u);
    TEST_ASSERT_EQUAL_INT(ZW_OTW_OK, p->result);
    TEST_ASSERT_EQUAL_UINT8(2u, p->attempts);
    run_for(5000u);

    setUp();
    n = make_gbl(700u);
    zac.fault.otw_cancel_block = 2u;
    zac.fault.otw_cancel_error = 0x18u; /* Zooz: 1.50/2.00 to 2.10 - go to 2.20 first */
    TEST_ASSERT_EQUAL_INT(ZW_OTW_BEGUN, zw_host_otw_begin(&host, wire.now, image, n));
    p = until_done(60000u);
    TEST_ASSERT_EQUAL_INT(ZW_OTW_ABORTED, p->result);
    TEST_ASSERT_EQUAL_HEX8(0x18u, p->error);
    TEST_ASSERT_EQUAL_UINT8(1u, p->attempts);
    run_for(10000u);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(&host));
}

/* ZW-025: a bootloader without "upload gbl" is not one the panel updates */
static void test_not_supported(void)
{
    const size_t n = make_gbl(300u);
    zac.fault.otw_no_upload = true;
    TEST_ASSERT_EQUAL_INT(ZW_OTW_BEGUN, zw_host_otw_begin(&host, wire.now, image, n));
    const zw_otw_progress_t *p = until_done(20000u);
    TEST_ASSERT_EQUAL_INT(ZW_OTW_NOT_SUPPORTED, p->result);
    run_for(10000u);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(&host));
}

/* ZW-025: firmware that doesn't start leaves the radio in its bootloader (the link down,
 * "bootloader"); the update can be tried again from there, and then it works */
static void test_again_from_the_bootloader(void)
{
    const size_t n = make_gbl(500u);
    zac.fault.otw_bad_image = true;
    TEST_ASSERT_EQUAL_INT(ZW_OTW_BEGUN, zw_host_otw_begin(&host, wire.now, image, n));
    const zw_otw_progress_t *p = until_done(60000u);
    TEST_ASSERT_EQUAL_INT(ZW_OTW_NOT_STARTED, p->result);
    run_for(15000u);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_DOWN, zw_host_link(&host));
    TEST_ASSERT_EQUAL_INT(ZW_DOWN_BOOTLOADER, zw_host_down_reason(&host)->reason);
    zac.fault.otw_bad_image = false;
    TEST_ASSERT_EQUAL_INT(ZW_OTW_BEGUN, zw_host_otw_begin(&host, wire.now, image, n));
    p = until_done(60000u);
    TEST_ASSERT_EQUAL_INT(ZW_OTW_OK, p->result);
    run_for(5000u);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(&host));
    TEST_ASSERT_EQUAL_UINT8(2u, heard.facts.firmware_major);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_update);
    RUN_TEST(test_bad_file);
    RUN_TEST(test_blocks_again_and_given_up);
    RUN_TEST(test_bootloader_refuses);
    RUN_TEST(test_not_supported);
    RUN_TEST(test_again_from_the_bootloader);
    return UNITY_END();
}
