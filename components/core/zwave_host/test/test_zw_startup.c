/*
 * test_zw_startup.c - the Z-Wave host's link to the ZAC93 (components/core/zwave_host,
 * docs/features/36 serial-api.md §4, §5, §7, §8 step 1): the start-up sequence against the fake
 * ZAC93 on a fake clock - the whole exchange for a ZAC93, byte for byte; each §4 refusal and the
 * protocol note; the region; step retries and §7's recovery (soft reset, reset line, link down)
 * and its 30 s / 5 min schedule; the module restarting by itself; the bootloader; requests held
 * behind the gate and failed when the link goes down. A jammed module (serial-api.md §6.12): a
 * frame it could not send at all goes again after 1 s, 5 times, then one soft reset an episode -
 * zw_net, its security manager and a simulated device against the fake ZAC93 (net_harness).
 *
 * Requirements: ZW-001 (the module found, and anything else refused with a reason), ZW-004
 * (start-up, again after a restart, recovery), ZW-037 (a frame the radio did not deliver)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_zac93.h"
#include "net_harness.h"
#include "unity.h"
#include "zwave_host/zw_host.h"

static zw_host_t host;
static zw_host_config_t cfg;
static fake_zac93_t zac;
static fake_wire_t wire;
static bool reset_wired;

/* ---- What the owner heard ----------------------------------------------------------------- */

static struct {
    unsigned ups, downs, restarts, frames, resets;
    uint64_t up_at, down_at[32];
    zw_link_down_t down[32];
    uint8_t restart_reason;
    zw_module_facts_t facts;
} heard;

/* The owner's own requests, as they ended. */
static struct {
    unsigned n;
    uint32_t tag[16];
    zw_tx_outcome_t status[16];
    uint64_t at[16];
} owner_done;

static void on_link_up(void *ctx, const zw_module_facts_t *facts)
{
    (void)ctx;
    heard.ups++;
    heard.up_at = wire.now;
    heard.facts = *facts;
}

static void on_link_down(void *ctx, const zw_link_down_t *why)
{
    (void)ctx;
    if (heard.downs < 32u) {
        heard.down[heard.downs] = *why;
        heard.down_at[heard.downs] = wire.now;
    }
    heard.downs++;
}

static void on_restarted(void *ctx, uint8_t reason)
{
    (void)ctx;
    heard.restarts++;
    heard.restart_reason = reason;
}

static void on_frame(void *ctx, const zw_frame_t *frame)
{
    (void)ctx;
    (void)frame;
    heard.frames++;
}

static bool port_write(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    return fake_wire_write(&wire, bytes, len);
}

static void port_reset_line(void *ctx, bool asserted)
{
    (void)ctx;
    if (asserted) {
        heard.resets++;
    }
    fake_zac93_reset_line(&zac, wire.now, asserted);
}

/* The wire's view of the host: zw_link's news and the zwave task's clock. */
static void host_control(void *ctx, uint64_t now, uint8_t b)
{
    zw_host_on_control(ctx, now, b);
}

static void host_frame(void *ctx, uint64_t now, const zw_frame_t *f)
{
    zw_host_on_frame(ctx, now, f);
}

static void host_text(void *ctx, uint64_t now, const uint8_t *b, size_t n)
{
    zw_host_on_text(ctx, now, b, n);
}

static void host_poll(void *ctx, uint64_t now)
{
    zw_host_poll(ctx, now);
}

static uint64_t host_deadline(void *ctx)
{
    return zw_host_next_deadline(ctx);
}

/* ---- The exchange, as text ---------------------------------------------------------------- */

static char exchange[16384];
static size_t exchange_len;
static unsigned sent[256]; /* frames the panel wrote, by function (retransmissions too) */

static void put(const char *s)
{
    const size_t n = strlen(s);
    TEST_ASSERT_TRUE_MESSAGE(exchange_len + n < sizeof exchange, "the exchange is too long");
    memcpy(&exchange[exchange_len], s, n + 1u);
    exchange_len += n;
}

/* What a frame is, for the reader of the exchange. */
static const char *frame_name(const uint8_t *b, size_t n)
{
    static char name[64];
    static const struct {
        uint8_t func;
        const char *name;
    } FUNCS[] = {
        {0x02, "GetSerialApiInitData"}, {0x03, "SetApplicationNodeInformation"},
        {0x05, "GetControllerCapabilities"}, {0x06, "SetSerialApiTimeouts"},
        {0x07, "GetSerialApiCapabilities"}, {0x08, "SoftReset"}, {0x09, "GetProtocolVersion"},
        {0x0A, "SerialAPIStarted"}, {0x0B, "SerialAPISetup"}, {0x13, "SendData"},
        {0x15, "GetControllerVersion"},
        {0x20, "GetControllerId"}, {0x54, "SetSUCNodeId"}, {0x56, "GetSUCNodeId"},
        {0xDA, "GetLongRangeNodes"},
    };
    static const struct {
        uint8_t sub;
        const char *name;
    } SUBS[] = {
        {0x00, "unsupported"}, {0x01, "GetSupportedCommands"}, {0x02, "SetTxStatusReport"},
        {0x10, "GetMaximumPayloadSize"}, {0x11, "GetLongRangeMaximumPayloadSize"},
        {0x15, "GetSupportedRegions"}, {0x16, "GetRegionInfo"}, {0x20, "GetRFRegion"},
        {0x40, "SetRFRegion"}, {0x80, "SetNodeIDType"},
    };
    const char *f = "?";
    const char *sub = NULL;
    for (size_t i = 0; i < sizeof FUNCS / sizeof FUNCS[0]; i++) {
        if (FUNCS[i].func == b[3]) {
            f = FUNCS[i].name;
        }
    }
    if (b[3] == 0x0B && n > 5) {
        for (size_t i = 0; i < sizeof SUBS / sizeof SUBS[0]; i++) {
            if (SUBS[i].sub == b[4]) {
                sub = SUBS[i].name;
            }
        }
    }
    snprintf(name, sizeof name, "%s%s%s%s", f, sub != NULL ? " " : "", sub != NULL ? sub : "",
             b[2] == 0x01 ? " (response)" : "");
    return name;
}

/* One unit of the wire on its lines: time, P> (the panel) or M> (the module), then an ACK, NAK
 * or CAN byte on the same line, or a frame's name (a run of text: "text") and its bytes below,
 * 20 to a line. */
static void put_unit(uint64_t at, bool to_module, const uint8_t *b, size_t n, bool frame)
{
    char line[96];
    snprintf(line, sizeof line, "%6llu %s ", (unsigned long long)at, to_module ? "P>" : "M>");
    put(line);
    if (!frame && n == 1 && (b[0] == 0x06 || b[0] == 0x15 || b[0] == 0x18)) {
        snprintf(line, sizeof line, "%02X\n", b[0]);
        put(line);
        return;
    }
    put(frame ? frame_name(b, n) : "text");
    for (size_t i = 0; i < n; i++) {
        snprintf(line, sizeof line, "%s%02X", i % 20 == 0 ? "\n         " : " ", b[i]);
        put(line);
    }
    put("\n");
}

static void tap(void *ctx, bool to_module, uint64_t at, const uint8_t *b, size_t n)
{
    (void)ctx;
    size_t i = 0;
    while (i < n) {
        if (b[i] == 0x01 && i + 1 < n && i + 2u + b[i + 1] <= n) { /* SOF LEN ... */
            const size_t len = 2u + b[i + 1];
            if (to_module && len > 3u) {
                sent[b[i + 3]]++;
            }
            put_unit(at, to_module, &b[i], len, true);
            i += len;
        } else if (b[i] == 0x06 || b[i] == 0x15 || b[i] == 0x18 || b[i] == 0x01) {
            put_unit(at, to_module, &b[i], 1, false);
            i++;
        } else { /* text, up to the next control byte */
            size_t j = i;
            while (j < n && b[j] != 0x01 && b[j] != 0x06 && b[j] != 0x15 && b[j] != 0x18) {
                j++;
            }
            put_unit(at, to_module, &b[i], j - i, false);
            i = j;
        }
    }
}

/* ---- Set-up ------------------------------------------------------------------------------- */

static void start_host(void)
{
    const zw_host_port_t port = {
        .ctx = NULL,
        .write = port_write,
        .reset_line = reset_wired ? port_reset_line : NULL,
        .link_up = on_link_up,
        .link_down = on_link_down,
        .restarted = on_restarted,
        .frame = on_frame,
    };
    zw_host_init(&host, &port, &cfg);
    zw_host_start(&host, wire.now);
}

void setUp(void)
{
    const fake_host_t fh = {&host, host_control, host_frame, host_text, host_poll, host_deadline};
    memset(&heard, 0, sizeof heard);
    memset(&owner_done, 0, sizeof owner_done);
    memset(&cfg, 0, sizeof cfg);
    exchange_len = 0u;
    exchange[0] = '\0';
    memset(sent, 0, sizeof sent);
    reset_wired = false;
    fake_zac93_init(&zac);
    fake_wire_init(&wire, &zac, &fh, 0u);
    wire.tap.bytes = tap;
}

void tearDown(void)
{
    TEST_ASSERT_FALSE_MESSAGE(wire.stuck, "the wire did not settle");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, zac.overflow, "the fake module ran out of room");
}

static void run(uint64_t t)
{
    fake_wire_run_until(&wire, t);
}

/* The exchange for the default identity (a ZAC93, firmware 1.50 on SDK 7.22.1, region USA, no
 * reset line, no region wish, Long Range off), frozen: see test_recorded_startup_exchange.
 * Checked against serial-api.md §5, step by step, and every frame's LEN and CHK recomputed:
 *   step 0   skipped: no reset line
 *   step 1   0 ms: a single NAK, then 500 ms of listening (no bootloader text)
 *   step 2   07 GetSerialApiCapabilities: 1.50, 027A/0004/0611, the function mask
 *   step 3   02 GetSerialApiInitData: API 0A, a primary controller, node 1, chip 08
 *   step 4   15 GetControllerVersion: "Z-Wave 7.22", library 01 (static controller)
 *   step 5   09 GetProtocolVersion: 7.22.1 (so no update note)
 *   step 6   05 GetControllerCapabilities: 28
 *   step 7   0B 01 GetSupportedCommands: 01 02 10 11 15 16 20 40 80
 *   step 8   0B 10 GetMaximumPayloadSize: 46 (0B 11 only with Long Range)
 *   step 9   0B 15 GetSupportedRegions (01, 09), 0B 16 GetRegionInfo for each, 0B 20: 01
 *   step 10  skipped: no region wish
 *   step 11  03 SetApplicationNodeInformation: listening, 01/00, 5E 9F 6C 56 8F (ACK only)
 *   step 12  08 SoftReset, then 20 ms later SerialAPIStarted (07; no Long Range)
 *   step 13  skipped: Long Range off
 *   step 14  0B 80 SetNodeIDType 01 (8-bit)
 *   step 15  20 GetControllerId: home C0FFEE01, node 1
 *   step 16  0B 02 SetTxStatusReport FF
 *   step 17  56 GetSUCNodeId: 0 - so 54 SetSUCNodeId(1, SUC, 25, SIS, callback 00): started,
 *            no callback when naming the controller itself (ZAC93 bench finding)
 *   step 18  06 SetSerialApiTimeouts A0 0F: the old values 96 0F
 *   step 19  link up at 520 ms */
static const char RECORDED_STARTUP[] =
    "     0 P> 15\n"
    "   500 P> GetSerialApiCapabilities\n"
    "         01 03 00 07 FB\n"
    "   500 M> 06\n"
    "   500 M> GetSerialApiCapabilities (response)\n"
    "         01 2B 01 07 01 32 02 7A 00 04 06 11 FE 87 34 80 40 20 00 14\n"
    "         E3 07 39 80 07 00 00 80 00 00 00 00 80 00 00 00 00 00 00 0F\n"
    "         00 00 00 00 66\n"
    "   500 P> 06\n"
    "   500 P> GetSerialApiInitData\n"
    "         01 03 00 02 FE\n"
    "   500 M> 06\n"
    "   500 M> GetSerialApiInitData (response)\n"
    "         01 25 01 02 0A 00 1D 01 00 00 00 00 00 00 00 00 00 00 00 00\n"
    "         00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 08 00 C7\n"
    "   500 P> 06\n"
    "   500 P> GetControllerVersion\n"
    "         01 03 00 15 E9\n"
    "   500 M> 06\n"
    "   500 M> GetControllerVersion (response)\n"
    "         01 10 01 15 5A 2D 57 61 76 65 20 37 2E 32 32 00 01 91\n"
    "   500 P> 06\n"
    "   500 P> GetProtocolVersion\n"
    "         01 03 00 09 F5\n"
    "   500 M> 06\n"
    "   500 M> GetProtocolVersion (response)\n"
    "         01 19 01 09 00 07 16 01 01 20 00 00 00 00 00 00 00 00 00 00\n"
    "         00 00 00 00 00 00 DF\n"
    "   500 P> 06\n"
    "   500 P> GetControllerCapabilities\n"
    "         01 03 00 05 F9\n"
    "   500 M> 06\n"
    "   500 M> GetControllerCapabilities (response)\n"
    "         01 04 01 05 28 D7\n"
    "   500 P> 06\n"
    "   500 P> SerialAPISetup GetSupportedCommands\n"
    "         01 04 00 0B 01 F1\n"
    "   500 M> 06\n"
    "   500 M> SerialAPISetup GetSupportedCommands (response)\n"
    "         01 15 01 0B 01 F3 03 80 31 80 00 00 00 80 00 00 00 00 00 00\n"
    "         00 80 20\n"
    "   500 P> 06\n"
    "   500 P> SerialAPISetup GetMaximumPayloadSize\n"
    "         01 04 00 0B 10 E0\n"
    "   500 M> 06\n"
    "   500 M> SerialAPISetup GetMaximumPayloadSize (response)\n"
    "         01 05 01 0B 10 2E CE\n"
    "   500 P> 06\n"
    "   500 P> SerialAPISetup GetSupportedRegions\n"
    "         01 04 00 0B 15 E5\n"
    "   500 M> 06\n"
    "   500 M> SerialAPISetup GetSupportedRegions (response)\n"
    "         01 07 01 0B 15 02 01 09 ED\n"
    "   500 P> 06\n"
    "   500 P> SerialAPISetup GetRegionInfo\n"
    "         01 05 00 0B 16 01 E6\n"
    "   500 M> 06\n"
    "   500 M> SerialAPISetup GetRegionInfo (response)\n"
    "         01 06 01 0B 16 01 01 E5\n"
    "   500 P> 06\n"
    "   500 P> SerialAPISetup GetRegionInfo\n"
    "         01 05 00 0B 16 09 EE\n"
    "   500 M> 06\n"
    "   500 M> SerialAPISetup GetRegionInfo (response)\n"
    "         01 07 01 0B 16 09 03 01 EF\n"
    "   500 P> 06\n"
    "   500 P> SerialAPISetup GetRFRegion\n"
    "         01 04 00 0B 20 D0\n"
    "   500 M> 06\n"
    "   500 M> SerialAPISetup GetRFRegion (response)\n"
    "         01 05 01 0B 20 01 D1\n"
    "   500 P> 06\n"
    "   500 P> SetApplicationNodeInformation\n"
    "         01 0C 00 03 01 01 00 05 5E 9F 6C 56 8F 81\n"
    "   500 M> 06\n"
    "   500 P> SoftReset\n"
    "         01 03 00 08 F4\n"
    "   500 M> 06\n"
    "   520 M> SerialAPIStarted\n"
    "         01 0F 00 0A 07 00 01 02 01 05 5E 9F 6C 56 8F 00 8E\n"
    "   520 P> 06\n"
    "   520 P> SerialAPISetup SetNodeIDType\n"
    "         01 05 00 0B 80 01 70\n"
    "   520 M> 06\n"
    "   520 M> SerialAPISetup SetNodeIDType (response)\n"
    "         01 05 01 0B 80 01 71\n"
    "   520 P> 06\n"
    "   520 P> GetControllerId\n"
    "         01 03 00 20 DC\n"
    "   520 M> 06\n"
    "   520 M> GetControllerId (response)\n"
    "         01 08 01 20 C0 FF EE 01 01 07\n"
    "   520 P> 06\n"
    "   520 P> SerialAPISetup SetTxStatusReport\n"
    "         01 05 00 0B 02 FF 0C\n"
    "   520 M> 06\n"
    "   520 M> SerialAPISetup SetTxStatusReport (response)\n"
    "         01 05 01 0B 02 01 F3\n"
    "   520 P> 06\n"
    "   520 P> GetSUCNodeId\n"
    "         01 03 00 56 AA\n"
    "   520 M> 06\n"
    "   520 M> GetSUCNodeId (response)\n"
    "         01 04 01 56 00 AC\n"
    "   520 P> 06\n"
    "   520 P> SetSUCNodeId\n"
    "         01 08 00 54 01 01 25 01 00 87\n"
    "   520 M> 06\n"
    "   520 M> SetSUCNodeId (response)\n"
    "         01 04 01 54 01 AF\n"
    "   520 P> 06\n"
    "   520 P> SetSerialApiTimeouts\n"
    "         01 05 00 06 A0 0F 53\n"
    "   520 M> 06\n"
    "   520 M> SetSerialApiTimeouts (response)\n"
    "         01 05 01 06 96 0F 64\n"
    "   520 P> 06\n"
    ;

/* ZW-001, ZW-004: the start-up exchange with a ZAC93 on SDK 7.22.1, every byte both ways, and
 * the facts the link comes up with. Regenerate with ZW_DUMP_STARTUP=1 build/host/test_zw_startup
 * (it prints the exchange as this constant) - and check it against serial-api.md §5 step by step
 * before freezing it again. */
static void test_recorded_startup_exchange(void)
{
    start_host();
    run(10000);
    if (getenv("ZW_DUMP_STARTUP") != NULL) {
        printf("static const char RECORDED_STARTUP[] =\n");
        for (const char *p = exchange; *p != '\0';) {
            const char *e = strchr(p, '\n');
            printf("    \"%.*s\\n\"\n", (int)(e - p), p);
            p = e + 1;
        }
        printf("    ;\n");
    }
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    TEST_ASSERT_EQUAL_STRING(RECORDED_STARTUP, exchange);
    TEST_ASSERT_EQUAL_UINT32(0u, zw_host_stats(&host)->recoveries);
}

/* ---- Helpers for the rest ----------------------------------------------------------------- */

/* The §5 timing the tests lean on, spelled out. */
#define LISTEN_MS        500u  /* step 1 */
#define SOFT_RESET_MS     20u  /* the fake's SoftReset to SerialAPIStarted */
#define BOOT_MS          200u  /* the fake's reset line (or "run") to SerialAPIStarted */
#define NO_ACK_MS       6000u  /* a command never ACKed: 1600 + 100 + 1600 + 1100 + 1600 ms */
#define RESPONSE_MS    10000u  /* a response that never comes */
#define UP_AT (LISTEN_MS + SOFT_RESET_MS) /* when a healthy start-up ends */

static void on_owner_done(void *ctx, uint32_t tag, const zw_tx_result_t *r)
{
    (void)ctx;
    if (owner_done.n < 16u) {
        owner_done.tag[owner_done.n] = tag;
        owner_done.status[owner_done.n] = r->status;
        owner_done.at[owner_done.n] = wire.now;
    }
    owner_done.n++;
}

/* A SendData to node 5 (Basic Set) from the owner, in class `prio`. */
static zw_submit_t submit_node_command(uint32_t tag, zw_prio_t prio)
{
    static const uint8_t payload[] = {0x05, 0x03, 0x20, 0x01, 0xFF, 0x25, 0x00};
    zw_request_t r;
    memset(&r, 0, sizeof r);
    r.func = ZW_FUNC_SEND_DATA;
    r.payload = payload;
    r.payload_len = sizeof payload;
    r.expect = ZW_EXPECT_RESPONSE | ZW_EXPECT_CALLBACK;
    r.callback_id_at = (uint8_t)(sizeof payload - 1u);
    r.prio = prio;
    r.done = on_owner_done;
    r.tag = tag;
    return zw_host_submit(&host, &r);
}

/* SendData answered: queued, then transmitted (callback status OK) 30 ms later. */
static void script_send_data(void)
{
    fake_rule_t r;
    memset(&r, 0, sizeof r);
    r.func = ZW_FUNC_SEND_DATA;
    r.respond = true;
    r.response.len = 1;
    r.response.bytes[0] = 0x01;
    r.callbacks = 1;
    r.callback[0].after_ms = 30;
    r.callback[0].len = 1;
    r.callback[0].bytes[0] = 0x00;
    TEST_ASSERT_TRUE(fake_zac93_on(&zac, &r));
}

/* How many frames with this FUNC (and, for SerialAPISetup, this sub-command) the module heard. */
static unsigned heard_func(uint8_t func, int sub)
{
    unsigned n = 0;
    for (size_t i = 0; i < zac.heard; i++) {
        const fake_heard_t *h = fake_zac93_heard(&zac, i);
        if (h != NULL && h->func == func &&
            (sub < 0 || (h->payload_len > 0 && h->payload[0] == sub))) {
            n++;
        }
    }
    return n;
}

/* The data byte of the last SerialAPISetup `sub` the module heard, or -1. */
static int heard_setup_data(uint8_t sub)
{
    int v = -1;
    for (size_t i = 0; i < zac.heard; i++) {
        const fake_heard_t *h = fake_zac93_heard(&zac, i);
        if (h != NULL && h->func == ZW_FUNC_SERIAL_API_SETUP && h->payload_len > 1u &&
            h->payload[0] == sub) {
            v = h->payload[1];
        }
    }
    return v;
}

static void clear_function(uint8_t func)
{
    zac.id.function_mask[(func - 1u) / 8u] =
        (uint8_t)(zac.id.function_mask[(func - 1u) / 8u] & ~(1u << ((func - 1u) % 8u)));
}

static void expect_down(unsigned i, zw_down_reason_t reason, uint8_t step, uint8_t detail)
{
    TEST_ASSERT_TRUE(heard.downs > i);
    TEST_ASSERT_EQUAL_INT(reason, heard.down[i].reason);
    TEST_ASSERT_EQUAL_UINT8(step, heard.down[i].step);
    TEST_ASSERT_EQUAL_UINT8(detail, heard.down[i].detail);
}

/* ---- The facts ---------------------------------------------------------------------------- */

/* ZW-001: the link comes up with the module's facts from every step (§5 steps 2-18), and the
 * gate open: the owner's command goes out at once. */
static void test_facts_at_link_up(void)
{
    script_send_data();
    start_host();
    run(UP_AT);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(&host));
    TEST_ASSERT_EQUAL_UINT64(UP_AT, heard.up_at);
    const zw_module_facts_t *f = &heard.facts;
    TEST_ASSERT_EQUAL_UINT8(1, f->firmware_major);
    TEST_ASSERT_EQUAL_UINT8(50, f->firmware_minor);
    TEST_ASSERT_EQUAL_HEX16(0x027A, f->manufacturer);
    TEST_ASSERT_EQUAL_HEX16(0x0004, f->product_type);
    TEST_ASSERT_EQUAL_HEX16(0x0611, f->product_id);
    TEST_ASSERT_TRUE(zw_func_mask_test(f->function_mask, ZW_FUNC_SEND_DATA));
    TEST_ASSERT_EQUAL_HEX8(0x0A, f->api_version);
    TEST_ASSERT_FALSE(f->is_secondary);
    TEST_ASSERT_TRUE(zw_node_list_test(f->node_list, 1));
    TEST_ASSERT_FALSE(zw_node_list_test(f->node_list, 2));
    TEST_ASSERT_EQUAL_HEX8(0x08, f->chip_type);
    TEST_ASSERT_EQUAL_STRING("Z-Wave 7.22", f->library);
    TEST_ASSERT_EQUAL_UINT8(1, f->library_type);
    TEST_ASSERT_EQUAL_UINT8(7, f->protocol_major);
    TEST_ASSERT_EQUAL_UINT8(22, f->protocol_minor);
    TEST_ASSERT_EQUAL_UINT8(1, f->protocol_patch);
    TEST_ASSERT_FALSE(f->update_note);
    TEST_ASSERT_TRUE(f->controller.was_real_primary && f->controller.no_nodes_included);
    TEST_ASSERT_EQUAL_UINT8(46, f->max_payload);
    TEST_ASSERT_EQUAL_UINT8(2, f->region_count);
    TEST_ASSERT_EQUAL_HEX8(0x01, f->regions[0]);
    TEST_ASSERT_EQUAL_HEX8(0x09, f->regions[1]);
    TEST_ASSERT_EQUAL_HEX8(0x01, f->region_flags[0]);
    TEST_ASSERT_EQUAL_HEX8(0x03, f->region_flags[1]);
    TEST_ASSERT_EQUAL_HEX8(0x01, f->region);
    TEST_ASSERT_FALSE(f->long_range_available);
    TEST_ASSERT_EQUAL_UINT8(8, f->node_id_bits);
    TEST_ASSERT_EQUAL_HEX32(0xC0FFEE01, f->home_id);
    TEST_ASSERT_EQUAL_UINT16(1, f->node_id);
    TEST_ASSERT_TRUE(f->tx_status_report);
    TEST_ASSERT_EQUAL_UINT16(1, f->suc_node_id);
    TEST_ASSERT_EQUAL_HEX8(0x96, f->old_ack_timeout);
    TEST_ASSERT_EQUAL_HEX8(0x0F, f->old_byte_timeout);
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_OK, submit_node_command(7, ZW_PRIO_NODE));
    run(UP_AT + 100u);
    TEST_ASSERT_EQUAL_UINT(1u, owner_done.n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, owner_done.status[0]);
}

/* ---- §4: what the panel refuses ----------------------------------------------------------- */

/* ZW-001: an end node (capabilities bit 0) is refused at step 3 and the link stays down; the
 * start-up is tried again 30 s later (the installer may swap the module) - and comes up when
 * the module has been swapped for a controller. */
static void test_refuses_end_node_then_swapped(void)
{
    zac.id.api_capabilities = 0x01u;
    start_host();
    run(LISTEN_MS);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_DOWN, zw_host_link(&host));
    TEST_ASSERT_EQUAL_UINT(1u, heard.downs);
    expect_down(0, ZW_DOWN_NOT_CONTROLLER, 3, 0);
    TEST_ASSERT_EQUAL_INT(ZW_DOWN_NOT_CONTROLLER, zw_host_down_reason(&host)->reason);
    TEST_ASSERT_EQUAL_UINT(0u, heard_func(ZW_FUNC_GET_CONTROLLER_VERSION, -1)); /* stopped */
    zac.id.api_capabilities = 0x00u; /* swapped */
    run(LISTEN_MS + 30000u - 1u);
    TEST_ASSERT_EQUAL_UINT(0u, heard.ups);
    run(LISTEN_MS + 30000u + UP_AT);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    TEST_ASSERT_EQUAL_UINT64(LISTEN_MS + 30000u + UP_AT, heard.up_at);
    TEST_ASSERT_EQUAL_UINT32(2u, zw_host_stats(&host)->attempts);
}

/* ZW-001: a chip that is not the 800 series is refused, naming the chip. */
static void test_refuses_wrong_chip(void)
{
    zac.id.chip_type = 0x07u;
    start_host();
    run(1000);
    expect_down(0, ZW_DOWN_WRONG_CHIP, 3, 0x07);
    TEST_ASSERT_EQUAL_UINT(0u, heard.ups);
}

/* ZW-001: a function §6 marks required missing from the mask is refused at step 2, naming it -
 * here SendData, 13. */
static void test_refuses_missing_function(void)
{
    clear_function(ZW_FUNC_SEND_DATA);
    start_host();
    run(1000);
    expect_down(0, ZW_DOWN_MISSING_FUNCTION, 2, ZW_FUNC_SEND_DATA);
    TEST_ASSERT_EQUAL_UINT(0u, heard_func(ZW_FUNC_GET_SERIAL_API_INIT_DATA, -1));
}

/* ZW-001: a library that is not a static (1) or bridge (7) controller is refused at step 4; a
 * bridge controller is accepted. */
static void test_refuses_wrong_library_accepts_bridge(void)
{
    zac.id.library_type = 0x02u; /* a plain controller */
    start_host();
    run(1000);
    expect_down(0, ZW_DOWN_WRONG_LIBRARY, 4, 0x02);
    zac.id.library_type = 0x07u; /* swapped for a bridge controller */
    run(LISTEN_MS + 30000u + UP_AT);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    TEST_ASSERT_EQUAL_UINT8(7, heard.facts.library_type);
}

/* ZW-001: an SDK before 7.22.1 starts, with the "update the radio's software" note (§4). */
static void test_old_protocol_note(void)
{
    zac.id.protocol[1] = 22u;
    zac.id.protocol[2] = 0u; /* 7.22.0 */
    start_host();
    run(UP_AT);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    TEST_ASSERT_TRUE(heard.facts.update_note);
    zac.id.protocol[1] = 21u;
    zac.id.protocol[2] = 9u; /* 7.21.9, after a start again (a region change, a reset) */
    zw_host_start(&host, wire.now);
    run(wire.now + UP_AT);
    TEST_ASSERT_EQUAL_UINT(2u, heard.ups);
    TEST_ASSERT_TRUE(heard.facts.update_note);
}

/* ---- The region (§5 steps 9, 10, 12) ------------------------------------------------------ */

/* ZW-004: the installer's region differs from the module's: SetRFRegion, and the soft reset
 * makes it take effect (here USA Long Range: SerialAPIStarted says Long Range is available). */
static void test_region_differs(void)
{
    cfg.set_region = true;
    cfg.region = 0x09u;
    start_host();
    run(UP_AT);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    TEST_ASSERT_EQUAL_UINT(1u, heard_func(ZW_FUNC_SERIAL_API_SETUP, ZW_SETUP_SET_RF_REGION));
    TEST_ASSERT_EQUAL_HEX8(0x09, heard.facts.region);
    TEST_ASSERT_EQUAL_HEX8(0x09, zac.id.region);
    TEST_ASSERT_TRUE(heard.facts.long_range_available);
    TEST_ASSERT_EQUAL_UINT8(8, heard.facts.node_id_bits); /* Long Range not wanted (phase 3) */
    TEST_ASSERT_EQUAL_UINT(0u, heard_func(ZW_FUNC_GET_LONG_RANGE_NODES, -1));
}

/* ZW-004: the same region: no SetRFRegion. */
static void test_region_same(void)
{
    cfg.set_region = true;
    cfg.region = 0x01u;
    start_host();
    run(UP_AT);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    TEST_ASSERT_EQUAL_UINT(0u, heard_func(ZW_FUNC_SERIAL_API_SETUP, ZW_SETUP_SET_RF_REGION));
    TEST_ASSERT_EQUAL_HEX8(0x01, heard.facts.region);
}

/* ZW-004: Long Range wanted in a Long Range region: the Long Range payload size (0B 11), the
 * Long Range node list (DA, step 13) and 16-bit node IDs (step 14), then GetControllerId read
 * with 2-byte node IDs. */
static void test_long_range_start(void)
{
    cfg.long_range = true;
    cfg.set_region = true;
    cfg.region = 0x09u;
    start_host();
    run(UP_AT);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    TEST_ASSERT_EQUAL_UINT8(150, heard.facts.lr_max_payload);
    TEST_ASSERT_EQUAL_UINT(1u, heard_func(ZW_FUNC_GET_LONG_RANGE_NODES, -1));
    TEST_ASSERT_EQUAL_INT(ZW_NID_16, heard_setup_data(ZW_SETUP_SET_NODE_ID_TYPE));
    TEST_ASSERT_EQUAL_UINT8(16, heard.facts.node_id_bits);
    TEST_ASSERT_EQUAL_UINT16(1, heard.facts.node_id);
    TEST_ASSERT_EQUAL_UINT16(1, heard.facts.suc_node_id);
}

/* ZW-004: Long Range wanted, but the module's region has none (USA, no region wish): the Long
 * Range payload size is read (step 8 is before the region is known), but SerialAPIStarted offers
 * no Long Range, so no Long Range node list (step 13) and 8-bit node IDs (step 14). */
static void test_long_range_wanted_region_without(void)
{
    cfg.long_range = true;
    start_host();
    run(UP_AT);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    TEST_ASSERT_EQUAL_UINT(1u, heard_func(ZW_FUNC_SERIAL_API_SETUP,
                                          ZW_SETUP_GET_LONG_RANGE_MAXIMUM_PAYLOAD_SIZE));
    TEST_ASSERT_FALSE(heard.facts.long_range_available);
    TEST_ASSERT_EQUAL_UINT(0u, heard_func(ZW_FUNC_GET_LONG_RANGE_NODES, -1));
    TEST_ASSERT_EQUAL_INT(ZW_NID_8, heard_setup_data(ZW_SETUP_SET_NODE_ID_TYPE));
    TEST_ASSERT_EQUAL_UINT8(8, heard.facts.node_id_bits);
}

/* ZW-004: a region the module does not have (02, not in its list): SetRFRegion is refused, the
 * module keeps its own, and the facts say so. */
static void test_region_refused(void)
{
    cfg.set_region = true;
    cfg.region = 0x02u;
    start_host();
    run(UP_AT);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    TEST_ASSERT_EQUAL_UINT(1u, heard_func(ZW_FUNC_SERIAL_API_SETUP, ZW_SETUP_SET_RF_REGION));
    TEST_ASSERT_EQUAL_HEX8(0x01, heard.facts.region);
    TEST_ASSERT_EQUAL_HEX8(0x01, zac.id.region);
}

/* ZW-004: Long Range wanted and available, but SetNodeIDType refused: 8-bit node IDs stay, and
 * GetControllerId and GetSUCNodeId are read with them. */
static void test_node_id_type_refused(void)
{
    cfg.long_range = true;
    cfg.set_region = true;
    cfg.region = 0x09u;
    zac.fault.refuse_setup = ZW_SETUP_SET_NODE_ID_TYPE;
    start_host();
    run(UP_AT);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    TEST_ASSERT_TRUE(heard.facts.long_range_available);
    TEST_ASSERT_EQUAL_INT(ZW_NID_16, heard_setup_data(ZW_SETUP_SET_NODE_ID_TYPE));
    TEST_ASSERT_EQUAL_UINT8(8, heard.facts.node_id_bits);
    TEST_ASSERT_EQUAL_HEX32(0xC0FFEE01, heard.facts.home_id);
    TEST_ASSERT_EQUAL_UINT16(1, heard.facts.node_id);
    TEST_ASSERT_EQUAL_UINT16(1, heard.facts.suc_node_id);
}

/* ---- The SUC (§5 step 17) ----------------------------------------------------------------- */

/* Starts, and checks that step 17 read the SUC (`suc`) and left it alone. */
static void expect_suc_left_alone(uint16_t suc)
{
    start_host();
    run(UP_AT);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    TEST_ASSERT_EQUAL_UINT(1u, heard_func(ZW_FUNC_GET_SUC_NODE_ID, -1));
    TEST_ASSERT_EQUAL_UINT(0u, heard_func(ZW_FUNC_SET_SUC_NODE_ID, -1));
    TEST_ASSERT_EQUAL_UINT16(suc, heard.facts.suc_node_id);
}

/* ZW-004: step 17 names the panel SUC and SIS only when there is no SUC, the panel is the
 * primary controller and no SIS is present (the recorded exchange); otherwise it leaves it. */
static void test_suc_left_alone(void)
{
    zac.id.suc_node_id = 5u; /* a SUC already */
    expect_suc_left_alone(5u);
    tearDown();
    setUp();
    zac.id.api_capabilities = 0x04u; /* a secondary controller, by GetSerialApiInitData */
    expect_suc_left_alone(0u);
    tearDown();
    setUp();
    zac.id.controller_caps = 0x29u; /* ...or by GetControllerCapabilities */
    expect_suc_left_alone(0u);
    tearDown();
    setUp();
    zac.id.controller_caps = 0x2Cu; /* the primary, with a SIS elsewhere */
    expect_suc_left_alone(0u);
}

/* ---- Step retries and §7 ------------------------------------------------------------------ */

/* ZW-004: a step that fails twice (no response, 10 s each) passes the third time (§5). */
static void test_step_fails_twice_then_passes(void)
{
    zac.fault.mute_func = ZW_FUNC_GET_CONTROLLER_VERSION;
    zac.fault.mute_count = 2u;
    start_host();
    run(LISTEN_MS + 2u * RESPONSE_MS - 1u);
    TEST_ASSERT_EQUAL_UINT(0u, heard.ups);
    run(LISTEN_MS + 2u * RESPONSE_MS + SOFT_RESET_MS);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    TEST_ASSERT_EQUAL_UINT(3u, heard_func(ZW_FUNC_GET_CONTROLLER_VERSION, -1));
    TEST_ASSERT_EQUAL_UINT32(2u, zw_host_stats(&host)->step_retries);
    TEST_ASSERT_EQUAL_UINT32(0u, zw_host_stats(&host)->recoveries);
    TEST_ASSERT_EQUAL_UINT(0u, heard.downs);
}

/* ZW-004: a step that fails three times is §7's: a soft reset, SerialAPIStarted, and the
 * start-up again from step 1 (steps 2-11 were not done) - which passes. */
static void test_step_fails_three_times_recovers(void)
{
    zac.fault.mute_func = ZW_FUNC_GET_CONTROLLER_VERSION;
    zac.fault.mute_count = 3u;
    start_host();
    const uint64_t reset_at = LISTEN_MS + 3u * RESPONSE_MS;
    run(reset_at);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_RECOVERING, zw_host_link(&host));
    TEST_ASSERT_EQUAL_UINT(1u, heard_func(ZW_FUNC_SOFT_RESET, -1));
    run(reset_at + SOFT_RESET_MS + LISTEN_MS + SOFT_RESET_MS);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    TEST_ASSERT_EQUAL_UINT64(reset_at + SOFT_RESET_MS + LISTEN_MS + SOFT_RESET_MS, heard.up_at);
    TEST_ASSERT_EQUAL_UINT32(1u, zw_host_stats(&host)->recoveries);
    TEST_ASSERT_EQUAL_UINT(2u, heard_func(ZW_FUNC_GET_SERIAL_API_CAPABILITIES, -1));
    TEST_ASSERT_EQUAL_UINT(0u, heard.downs);
}

/* ZW-004: ...and when it fails three times again after the soft reset, the link goes down,
 * naming the step (no answer: not answering). */
static void test_step_keeps_failing_link_down(void)
{
    zac.fault.mute_func = ZW_FUNC_GET_CONTROLLER_VERSION;
    zac.fault.mute_count = FAKE_MUTE_FOREVER;
    start_host();
    const uint64_t reset_at = LISTEN_MS + 3u * RESPONSE_MS;
    const uint64_t down_at = reset_at + SOFT_RESET_MS + LISTEN_MS + 3u * RESPONSE_MS;
    run(down_at - 1u);
    TEST_ASSERT_EQUAL_UINT(0u, heard.downs);
    run(down_at);
    TEST_ASSERT_EQUAL_UINT(1u, heard.downs);
    expect_down(0, ZW_DOWN_NOT_ANSWERING, 4, 0);
    TEST_ASSERT_EQUAL_UINT64(down_at, heard.down_at[0]);
}

/* ZW-004: an answer the panel cannot use (SetSUCNodeId's response says it failed) is a failing
 * step like no answer: three tries, the soft reset (steps 13-19 again after it), three more
 * tries, then the link goes down - a bad answer, at step 17. */
static void test_bad_answer_link_down(void)
{
    fake_rule_t r;
    memset(&r, 0, sizeof r);
    r.func = ZW_FUNC_SET_SUC_NODE_ID;
    r.respond = true;
    r.response.len = 1;
    r.response.bytes[0] = 0x00; /* not started; no callback for its own node */
    TEST_ASSERT_TRUE(fake_zac93_on(&zac, &r));
    start_host();
    run(UP_AT + 2u * SOFT_RESET_MS);
    TEST_ASSERT_EQUAL_UINT(0u, heard.ups);
    TEST_ASSERT_EQUAL_UINT(1u, heard.downs);
    TEST_ASSERT_EQUAL_UINT64(UP_AT + SOFT_RESET_MS, heard.down_at[0]);
    expect_down(0, ZW_DOWN_BAD_ANSWER, 17, 0);
    TEST_ASSERT_EQUAL_UINT(6u, heard_func(ZW_FUNC_SET_SUC_NODE_ID, -1));
    TEST_ASSERT_EQUAL_UINT(2u, heard_func(ZW_FUNC_GET_SUC_NODE_ID, -1));
    TEST_ASSERT_EQUAL_UINT(1u, heard_func(ZW_FUNC_GET_SERIAL_API_CAPABILITIES, -1));
    TEST_ASSERT_EQUAL_UINT32(1u, zw_host_stats(&host)->recoveries);
}

/* ZW-004: a hung module and a reset line: step 0 pulses it (20 ms low), the module comes back
 * (SerialAPIStarted), then step 1 and the rest. */
static void test_reset_line_revives_hung_module_at_start(void)
{
    reset_wired = true;
    zac.fault.silent = true;
    start_host();
    TEST_ASSERT_TRUE(zac.reset_held);
    run(19);
    TEST_ASSERT_TRUE(zac.reset_held);
    run(20);
    TEST_ASSERT_FALSE(zac.reset_held);
    const uint64_t up = 20u + BOOT_MS + LISTEN_MS + SOFT_RESET_MS;
    run(up);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    TEST_ASSERT_EQUAL_UINT64(up, heard.up_at);
    TEST_ASSERT_EQUAL_UINT(1u, heard.resets);
}

/* ZW-004: the module hangs while up. The owner's SendData gets no ACK (3 transmissions), so §7:
 * a soft reset (no ACK either), then the reset line, SerialAPIStarted, steps 13-19 again - the
 * link is up once more, no link down on the way; the owner's command failed, NO_ACK. */
static void test_hang_while_up_recovered_by_reset_line(void)
{
    reset_wired = true;
    start_host();
    const uint64_t up = 20u + BOOT_MS + LISTEN_MS + SOFT_RESET_MS;
    run(up);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    zac.fault.silent = true;
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_OK, submit_node_command(9, ZW_PRIO_NODE));
    run(up + NO_ACK_MS);
    TEST_ASSERT_EQUAL_UINT(1u, owner_done.n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_NO_ACK, owner_done.status[0]);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_RECOVERING, zw_host_link(&host));
    const uint64_t pulse = up + 2u * NO_ACK_MS; /* after the soft reset's own 3 transmissions */
    run(pulse + 20u + BOOT_MS);
    TEST_ASSERT_EQUAL_UINT(2u, heard.resets);
    TEST_ASSERT_EQUAL_UINT(2u, heard.ups);
    TEST_ASSERT_EQUAL_UINT64(pulse + 20u + BOOT_MS, heard.up_at);
    TEST_ASSERT_EQUAL_UINT(0u, heard.downs);
    TEST_ASSERT_EQUAL_UINT32(1u, zw_host_stats(&host)->recoveries);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(&host));
}

/* ZW-004: a silent module and no reset line: step 2 fails three times (no ACK), the soft reset
 * gets no ACK either, and the link goes down - not answering, at step 2. */
static void test_silent_without_reset_line_goes_down(void)
{
    zac.fault.silent = true;
    start_host();
    const uint64_t down = LISTEN_MS + 3u * NO_ACK_MS + NO_ACK_MS;
    run(down - 1u);
    TEST_ASSERT_EQUAL_UINT(0u, heard.downs);
    run(down);
    TEST_ASSERT_EQUAL_UINT(1u, heard.downs);
    expect_down(0, ZW_DOWN_NOT_ANSWERING, 2, 0);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_DOWN, zw_host_link(&host));
    TEST_ASSERT_EQUAL_UINT(0u, heard.resets);
}

/* ZW-004: a module the reset line cannot revive (broken, or not plugged in). Step 0's pulse gets
 * no SerialAPIStarted in 3 s, so step 1 all the same; step 2 fails three times, the soft reset
 * gets no ACK, the reset line is pulsed again, and 3 s later the link goes down - not answering,
 * at step 2. The next attempt, 30 s on, starts with the reset line again. */
static void test_dead_module_reset_line_then_down(void)
{
    reset_wired = true;
    zac.fault.dead = true;
    start_host();
    const uint64_t step1 = 20u + 3000u; /* the pulse, then 3 s without SerialAPIStarted */
    const uint64_t pulse = step1 + LISTEN_MS + 3u * NO_ACK_MS + NO_ACK_MS; /* step 2, SoftReset */
    const uint64_t down = pulse + 20u + 3000u;
    run(down - 1u);
    TEST_ASSERT_EQUAL_UINT(0u, heard.downs);
    TEST_ASSERT_EQUAL_UINT(2u, heard.resets);
    TEST_ASSERT_NOT_NULL(strstr(exchange, "  3020 P> 15\n")); /* step 1's NAK */
    TEST_ASSERT_EQUAL_UINT(3u * 3u, sent[ZW_FUNC_GET_SERIAL_API_CAPABILITIES]);
    TEST_ASSERT_EQUAL_UINT(3u, sent[ZW_FUNC_SOFT_RESET]);
    run(down);
    TEST_ASSERT_EQUAL_UINT(1u, heard.downs);
    TEST_ASSERT_EQUAL_UINT64(down, heard.down_at[0]);
    expect_down(0, ZW_DOWN_NOT_ANSWERING, 2, 0);
    run(down + 30000u - 1u);
    TEST_ASSERT_EQUAL_UINT(2u, heard.resets);
    run(down + 30000u);
    TEST_ASSERT_EQUAL_UINT(3u, heard.resets); /* the next attempt's step 0 */
}

/* ZW-004: while down, the whole start-up again 30 s after each failure, 10 times, then every
 * 5 minutes; the counters start again when the link has been up. */
static void test_retry_schedule(void)
{
    zac.fault.silent = true;
    start_host();
    const uint64_t attempt = LISTEN_MS + 4u * NO_ACK_MS; /* one failing attempt, start to down */
    uint64_t t = attempt;
    for (unsigned k = 1; k <= 12; k++) {
        run(t);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(k, heard.downs, "downs");
        TEST_ASSERT_EQUAL_UINT64(t, heard.down_at[k - 1]);
        t += (k <= 10 ? 30000u : 300000u) + attempt;
    }
    TEST_ASSERT_EQUAL_UINT32(12u, zw_host_stats(&host)->attempts);
    /* the module comes back before the 13th attempt, which brings the link up */
    zac.fault.silent = false;
    const uint64_t attempt13 = heard.down_at[11] + 300000u;
    run(attempt13 + UP_AT);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    /* it hangs again: after this failure the wait is 30 s once more */
    zac.fault.silent = true;
    script_send_data();
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_OK, submit_node_command(1, ZW_PRIO_NODE));
    const uint64_t down = attempt13 + UP_AT + 2u * NO_ACK_MS; /* SendData, then the soft reset */
    run(down);
    TEST_ASSERT_EQUAL_UINT(13u, heard.downs);
    TEST_ASSERT_EQUAL_UINT64(down, heard.down_at[12]);
    expect_down(12, ZW_DOWN_NOT_ANSWERING, 0, 0);
    run(down + 30000u - 1u);
    TEST_ASSERT_EQUAL_UINT32(13u, zw_host_stats(&host)->attempts);
    run(down + 30000u);
    TEST_ASSERT_EQUAL_UINT32(14u, zw_host_stats(&host)->attempts);
}

/* ---- Restarts (§2, §7) -------------------------------------------------------------------- */

static void submit_two_on_restart(void *ctx, uint8_t reason)
{
    on_restarted(ctx, reason);
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_OK, submit_node_command(21, ZW_PRIO_NODE));
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_OK, submit_node_command(22, ZW_PRIO_REPLY));
}

/* ZW-004: the module restarts by itself while up (watchdog): the command in flight fails, the
 * owner hears "restarted", the gate closes, SerialAPIStarted counts as step 12's answer and
 * steps 13-19 run again - the owner's node commands, queued meanwhile, wait behind the gate and
 * go once the link is up again, the reply first. */
static void test_unexpected_restart_while_up(void)
{
    const zw_host_port_t port = {NULL, port_write, NULL, on_link_up, on_link_down,
                                 submit_two_on_restart, on_frame, NULL};
    fake_rule_t r;
    memset(&r, 0, sizeof r);
    r.func = ZW_FUNC_SEND_DATA;
    r.respond = true;
    r.response.after_ms = 10; /* slow enough to be in flight when the module restarts */
    r.response.len = 1;
    r.response.bytes[0] = 0x01;
    r.callbacks = 1;
    r.callback[0].after_ms = 40;
    r.callback[0].len = 1;
    TEST_ASSERT_TRUE(fake_zac93_on(&zac, &r));
    zw_host_init(&host, &port, &cfg);
    zw_host_start(&host, 0u);
    run(1000);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_OK, submit_node_command(20, ZW_PRIO_NODE));
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&zac, 1005u, 0x03u)); /* watchdog */
    run(1005);
    TEST_ASSERT_EQUAL_UINT(1u, heard.restarts);
    TEST_ASSERT_EQUAL_HEX8(0x03, heard.restart_reason);
    TEST_ASSERT_EQUAL_UINT(1u, owner_done.n);
    TEST_ASSERT_EQUAL_UINT32(20u, owner_done.tag[0]);
    TEST_ASSERT_EQUAL_INT(ZW_TX_MODULE_RESTARTED, owner_done.status[0]);
    run(2000);
    TEST_ASSERT_EQUAL_UINT(2u, heard.ups);
    TEST_ASSERT_EQUAL_UINT(0u, heard.downs);
    TEST_ASSERT_EQUAL_UINT(3u, owner_done.n);
    TEST_ASSERT_EQUAL_UINT32(22u, owner_done.tag[1]); /* the nonce reply before the command */
    TEST_ASSERT_EQUAL_UINT32(21u, owner_done.tag[2]);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, owner_done.status[1]);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, owner_done.status[2]);
    /* steps 13-19 ran again, and the held commands went only after them (all at 1005 ms: the
     * order is what counts) - 20 went before the restart, 22 and 21 after step 18 */
    TEST_ASSERT_EQUAL_UINT(2u, heard_func(ZW_FUNC_GET_CONTROLLER_ID, -1));
    TEST_ASSERT_EQUAL_UINT(2u, heard_func(ZW_FUNC_SET_SERIAL_API_TIMEOUTS, -1));
    TEST_ASSERT_EQUAL_UINT(1u, heard_func(ZW_FUNC_GET_SERIAL_API_CAPABILITIES, -1));
    size_t last_timeouts = 0;
    for (size_t i = 0; i < zac.heard; i++) {
        if (fake_zac93_heard(&zac, i)->func == ZW_FUNC_SET_SERIAL_API_TIMEOUTS) {
            last_timeouts = i;
        }
    }
    unsigned send_data = 0;
    for (size_t i = 0; i < zac.heard; i++) {
        if (fake_zac93_heard(&zac, i)->func == ZW_FUNC_SEND_DATA && ++send_data > 1u) {
            TEST_ASSERT_TRUE_MESSAGE(i > last_timeouts, "a held command went before step 18");
        }
    }
    TEST_ASSERT_EQUAL_UINT(3u, send_data);
    TEST_ASSERT_EQUAL_UINT32(1u, zw_host_stats(&host)->restarts);
}

/* ZW-004: a restart while starting, past step 12 (GetControllerId unanswered meanwhile): steps
 * 13-19 again, with no "restarted" for the owner (the link was not up). */
static void test_restart_while_starting(void)
{
    zac.fault.mute_func = ZW_FUNC_GET_CONTROLLER_ID;
    zac.fault.mute_count = 1u;
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&zac, 3000u, 0x09u)); /* brown-out */
    start_host();
    run(3000 + UP_AT);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    TEST_ASSERT_EQUAL_UINT64(3000u, heard.up_at);
    TEST_ASSERT_EQUAL_UINT(0u, heard.restarts);
    TEST_ASSERT_EQUAL_UINT(2u, heard_func(ZW_FUNC_GET_CONTROLLER_ID, -1));
    TEST_ASSERT_EQUAL_UINT(1u, heard_func(ZW_FUNC_SOFT_RESET, -1));
}

/* ZW-004: a restart while starting, before step 12 (GetControllerVersion unanswered meanwhile):
 * the same command again - steps 2-11 only read the module or set what step 12's soft reset
 * applies - and on to link up; not a failed try, and nothing for the owner. */
static void test_restart_before_step_12(void)
{
    zac.fault.mute_func = ZW_FUNC_GET_CONTROLLER_VERSION;
    zac.fault.mute_count = 1u;
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&zac, 2000u, 0x09u)); /* brown-out */
    start_host();
    run(2000u + SOFT_RESET_MS);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    TEST_ASSERT_EQUAL_UINT64(2000u + SOFT_RESET_MS, heard.up_at);
    TEST_ASSERT_EQUAL_UINT(0u, heard.restarts);
    TEST_ASSERT_EQUAL_UINT(2u, heard_func(ZW_FUNC_GET_CONTROLLER_VERSION, -1));
    TEST_ASSERT_EQUAL_UINT(1u, heard_func(ZW_FUNC_GET_SERIAL_API_CAPABILITIES, -1));
    TEST_ASSERT_EQUAL_UINT32(0u, zw_host_stats(&host)->step_retries);
}

/* ZW-004: the start-up begun again (a region change) while one of its commands waits for its
 * answer: that command's end, when it comes, belongs to the old attempt and is ignored; the new
 * attempt goes on from step 0 once the transport is free. */
static void test_start_again_mid_step(void)
{
    zac.fault.mute_func = ZW_FUNC_GET_CONTROLLER_VERSION;
    zac.fault.mute_count = 1u;
    start_host();
    run(1000);
    zw_host_start(&host, wire.now);
    const uint64_t freed = LISTEN_MS + RESPONSE_MS; /* the old GetControllerVersion's timeout */
    run(freed + SOFT_RESET_MS);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    TEST_ASSERT_EQUAL_UINT64(freed + SOFT_RESET_MS, heard.up_at);
    TEST_ASSERT_EQUAL_UINT(2u, heard_func(ZW_FUNC_GET_SERIAL_API_CAPABILITIES, -1));
    TEST_ASSERT_EQUAL_UINT(2u, heard_func(ZW_FUNC_GET_CONTROLLER_VERSION, -1));
    TEST_ASSERT_EQUAL_UINT32(0u, zw_host_stats(&host)->step_retries);
    TEST_ASSERT_EQUAL_UINT32(2u, zw_host_stats(&host)->attempts);
}

/* ZW-004: a module that keeps restarting while starting: three restarts are borne (steps 13-19
 * again each time), at the fourth the link goes down. */
static void test_restarting_again_and_again(void)
{
    zac.fault.mute_func = ZW_FUNC_GET_CONTROLLER_ID;
    zac.fault.mute_count = FAKE_MUTE_FOREVER;
    for (unsigned i = 1; i <= 4; i++) {
        TEST_ASSERT_TRUE(fake_zac93_restart_at(&zac, 2000u * i, 0x03u));
    }
    start_host();
    run(7999);
    TEST_ASSERT_EQUAL_UINT(0u, heard.downs);
    TEST_ASSERT_EQUAL_UINT(4u, heard_func(ZW_FUNC_GET_CONTROLLER_ID, -1));
    run(8000);
    TEST_ASSERT_EQUAL_UINT(1u, heard.downs);
    TEST_ASSERT_EQUAL_UINT64(8000u, heard.down_at[0]);
    expect_down(0, ZW_DOWN_RESTARTING, 15, 0);
    TEST_ASSERT_EQUAL_UINT(0u, heard.restarts); /* the link was never up */
}

/* ---- The bootloader (§5 step 1, §8 step 1) ------------------------------------------------ */

/* ZW-004: the module sits in its bootloader: step 1's NAK brings the menu; the host picks
 * "run" by its text (2), SerialAPIStarted follows, and the start-up begins again at step 1. */
static void test_bootloader_run(void)
{
    zac.fault.bootloader = true;
    start_host();
    run(LISTEN_MS);
    TEST_ASSERT_FALSE(zac.fault.bootloader); /* "run" chosen at 500 ms */
    const uint64_t up = LISTEN_MS + BOOT_MS + LISTEN_MS + SOFT_RESET_MS;
    run(up);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    TEST_ASSERT_EQUAL_UINT64(up, heard.up_at);
    TEST_ASSERT_EQUAL_UINT32(1u, zw_host_stats(&host)->bootloader);
    TEST_ASSERT_NOT_NULL(strstr(exchange, "   500 P> text\n         32\n")); /* '2' */
    TEST_ASSERT_NOT_NULL(strstr(exchange, "     0 M> text\n")); /* the menu */
}

/* ZW-004: a module that falls back into its bootloader every time: two escapes, then the link
 * goes down - stuck in the bootloader, step 1. */
static void test_bootloader_stuck(void)
{
    zac.fault.bootloader = true;
    zac.fault.bootloader_sticky = true;
    start_host();
    run(10000);
    TEST_ASSERT_EQUAL_UINT(0u, heard.ups);
    TEST_ASSERT_EQUAL_UINT(1u, heard.downs);
    expect_down(0, ZW_DOWN_BOOTLOADER, 1, 0);
    TEST_ASSERT_EQUAL_UINT32(2u, zw_host_stats(&host)->bootloader);
}

/* ZW-004: "run" chosen, but the application does not start within 5 s: the link goes down -
 * stuck in the bootloader. When the module then starts after all (SerialAPIStarted while the
 * link is down), the start-up runs at once, not at the next 30 s. */
static void test_bootloader_run_never_starts(void)
{
    zac.fault.bootloader = true;
    zac.boot_ms = 8000u; /* "run" to SerialAPIStarted */
    start_host();
    const uint64_t down = LISTEN_MS + 5000u;
    run(down - 1u);
    TEST_ASSERT_EQUAL_UINT(0u, heard.downs);
    run(down);
    TEST_ASSERT_EQUAL_UINT(1u, heard.downs);
    expect_down(0, ZW_DOWN_BOOTLOADER, 1, 0);
    const uint64_t started = LISTEN_MS + 8000u;
    run(started + UP_AT);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    TEST_ASSERT_EQUAL_UINT64(started + UP_AT, heard.up_at);
    TEST_ASSERT_EQUAL_UINT32(2u, zw_host_stats(&host)->attempts);
}

/* ZW-004, serial-api.md §5 step 1: a bootloader menu that arrives in pieces - "\r\nGeck", then
 * the rest 20 ms later - is still the bootloader's. Mirrors Z-Wave JS's bootloaderDetection.test.ts
 * ("the bootloader is detected when received in smaller chunks", their issue #7316): "run" chosen
 * at the end of the listening window, then the start-up again. */
static void test_bootloader_banner_in_pieces(void)
{
    zac.fault.bootloader = true;
    zac.fault.menu_split = 6u;
    zac.fault.menu_gap_ms = 20u;
    start_host();
    run(LISTEN_MS);
    TEST_ASSERT_FALSE(zac.fault.bootloader); /* "run" chosen at 500 ms */
    const uint64_t up = LISTEN_MS + BOOT_MS + LISTEN_MS + SOFT_RESET_MS;
    run(up);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    TEST_ASSERT_EQUAL_UINT64(up, heard.up_at);
    TEST_ASSERT_EQUAL_UINT32(1u, zw_host_stats(&host)->bootloader);
    TEST_ASSERT_NOT_NULL(strstr(exchange, "     0 M> text\n         0D 0A 47 65 63 6B\n"));
    TEST_ASSERT_NOT_NULL(strstr(exchange, "    20 M> text\n"));
}

/* A SendData whose callback comes 3 s after "queued": in flight a good while. */
static void script_slow_send_data(void)
{
    fake_rule_t r;
    memset(&r, 0, sizeof r);
    r.func = ZW_FUNC_SEND_DATA;
    r.respond = true;
    r.response.len = 1;
    r.response.bytes[0] = 0x01;
    r.callbacks = 1;
    r.callback[0].after_ms = 3000;
    r.callback[0].len = 1;
    TEST_ASSERT_TRUE(fake_zac93_on(&zac, &r));
}

/* ZW-004: the module crashes into its bootloader while the link is up - the Gecko bootloader's
 * menu arrives unasked, in two reads 20 ms apart, while the owner's SendData waits for its
 * callback. Mirrors Z-Wave JS's driverReadyStaleness.test.ts ("driver.ready becomes false when the
 * bootloader is unexpectedly detected"). It is recognised the moment the menu is whole, and for
 * what it is - not seconds later by commands going unanswered: the SendData ends at once
 * (MODULE_RESTARTED), and nothing more of it (no retransmission, no SendDataAbort) reaches the
 * bootloader, where every byte is a key; the owner hears "restarted" (reason FF); the host picks
 * "run" straight away and, at its SerialAPIStarted, starts again from step 1 - the link up once
 * more, never down. */
static void test_bootloader_while_up_recognised_at_once(void)
{
    script_slow_send_data();
    start_host();
    run(UP_AT);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_OK, submit_node_command(30, ZW_PRIO_NODE));
    const uint64_t crash = UP_AT + 100u;
    run(crash);
    TEST_ASSERT_EQUAL_UINT(0u, owner_done.n); /* waiting for its callback */
    zac.fault.menu_split = 6u; /* "\r\nGeck", then the rest */
    zac.fault.menu_gap_ms = 20u;
    fake_zac93_enter_bootloader(&zac, wire.now);
    run(crash + 19u);
    TEST_ASSERT_EQUAL_UINT(0u, heard.restarts); /* half a banner is no banner yet */
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(&host));
    run(crash + 20u);
    TEST_ASSERT_EQUAL_UINT(1u, heard.restarts);
    TEST_ASSERT_EQUAL_HEX8(ZW_HOST_RESTART_BOOTLOADER, heard.restart_reason);
    TEST_ASSERT_EQUAL_UINT(1u, owner_done.n);
    TEST_ASSERT_EQUAL_UINT32(30u, owner_done.tag[0]);
    TEST_ASSERT_EQUAL_INT(ZW_TX_MODULE_RESTARTED, owner_done.status[0]);
    TEST_ASSERT_EQUAL_UINT64(crash + 20u, owner_done.at[0]);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_RECOVERING, zw_host_link(&host));
    TEST_ASSERT_EQUAL_UINT32(1u, zw_host_stats(&host)->bootloader_falls);
    TEST_ASSERT_EQUAL_UINT32(1u, zw_host_stats(&host)->bootloader);
    char run_key[48];
    snprintf(run_key, sizeof run_key, "%6llu P> text\n         32\n",
             (unsigned long long)(crash + 20u));
    TEST_ASSERT_NOT_NULL(strstr(exchange, run_key));
    TEST_ASSERT_EQUAL_UINT(1u, sent[ZW_FUNC_SEND_DATA]);
    TEST_ASSERT_EQUAL_UINT(0u, sent[ZW_FUNC_SEND_DATA_ABORT]);
    const uint64_t up = crash + 20u + BOOT_MS + LISTEN_MS + SOFT_RESET_MS;
    run(up);
    TEST_ASSERT_EQUAL_UINT(2u, heard.ups);
    TEST_ASSERT_EQUAL_UINT64(up, heard.up_at);
    TEST_ASSERT_EQUAL_UINT(0u, heard.downs);
    TEST_ASSERT_EQUAL_UINT(2u, heard_func(ZW_FUNC_GET_SERIAL_API_CAPABILITIES, -1)); /* step 2 again */
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(&host));
}

/* ZW-004: the same crash into a bootloader the application keeps falling back into: two escapes,
 * then the link goes down - "stuck in the bootloader" (at step 1), not "not answering". Stray
 * text heard earlier while up (more than the host keeps) does not hide the menu. */
static void test_bootloader_while_up_stuck_goes_down(void)
{
    start_host();
    run(UP_AT);
    uint8_t stray[200];
    memset(stray, '.', sizeof stray);
    TEST_ASSERT_TRUE(fake_zac93_send_raw_at(&zac, wire.now + 1u, stray, sizeof stray));
    TEST_ASSERT_TRUE(fake_zac93_send_raw_at(&zac, wire.now + 2u, stray, sizeof stray));
    run(UP_AT + 100u);
    TEST_ASSERT_EQUAL_UINT(0u, heard.restarts);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(&host));
    zac.fault.bootloader_sticky = true;
    fake_zac93_enter_bootloader(&zac, wire.now);
    run(UP_AT + 10000u);
    TEST_ASSERT_EQUAL_UINT(1u, heard.restarts);
    TEST_ASSERT_EQUAL_UINT(1u, heard.downs);
    expect_down(0, ZW_DOWN_BOOTLOADER, 1, 0);
    TEST_ASSERT_EQUAL_UINT32(2u, zw_host_stats(&host)->bootloader);
    TEST_ASSERT_EQUAL_UINT32(1u, zw_host_stats(&host)->bootloader_falls);
}

/* ---- The gate and link down --------------------------------------------------------------- */

/* ZW-004: link down fails the owner's queued requests (every class that needs the link) with
 * LINK_DOWN; while down, new ones are refused; module commands are the host's own. */
static void test_link_down_fails_queued_requests(void)
{
    start_host();
    run(UP_AT);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    zac.fault.silent = true;
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_OK, submit_node_command(1, ZW_PRIO_NODE)); /* goes first */
    run(UP_AT + 1u);
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_OK, submit_node_command(2, ZW_PRIO_INTERVIEW));
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_OK, submit_node_command(3, ZW_PRIO_BACKGROUND));
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_OK, submit_node_command(4, ZW_PRIO_REPLY));
    const uint64_t down = UP_AT + 2u * NO_ACK_MS; /* the command, then the soft reset */
    run(down);
    TEST_ASSERT_EQUAL_UINT(1u, heard.downs);
    TEST_ASSERT_EQUAL_UINT(4u, owner_done.n);
    TEST_ASSERT_EQUAL_UINT32(1u, owner_done.tag[0]);
    TEST_ASSERT_EQUAL_INT(ZW_TX_NO_ACK, owner_done.status[0]);
    static const uint32_t order[] = {4, 2, 3}; /* in the order they would have gone */
    for (unsigned i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_UINT32(order[i], owner_done.tag[1 + i]);
        TEST_ASSERT_EQUAL_INT(ZW_TX_LINK_DOWN, owner_done.status[1 + i]);
        TEST_ASSERT_EQUAL_UINT64(down, owner_done.at[1 + i]);
    }
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_LINK_DOWN, submit_node_command(5, ZW_PRIO_NODE));
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_INVALID, submit_node_command(6, ZW_PRIO_MODULE));
    /* Only the first went, three times unanswered; the others never reached the wire. */
    TEST_ASSERT_EQUAL_UINT(3u, sent[ZW_FUNC_SEND_DATA]);
}

/* ZW-004: while the link is starting, the owner's requests wait behind the gate (a module
 * command from the owner, or one with the host's tag, is refused), and go at link up. */
static void test_requests_wait_while_starting(void)
{
    const zw_host_port_t port = {NULL, port_write, NULL, on_link_up, on_link_down,
                                 on_restarted, on_frame, NULL};
    script_send_data();
    zw_host_init(&host, &port, &cfg);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_OFF, zw_host_link(&host));
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_LINK_DOWN, submit_node_command(1, ZW_PRIO_REPLY));
    zw_host_start(&host, 0u);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_STARTING, zw_host_link(&host));
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_OK, submit_node_command(1, ZW_PRIO_REPLY));
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_INVALID, submit_node_command(0x5A570001u, ZW_PRIO_NODE));
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_INVALID, submit_node_command(2, ZW_PRIO_MODULE));
    run(UP_AT - 1u);
    TEST_ASSERT_EQUAL_UINT(0u, heard_func(ZW_FUNC_SEND_DATA, -1));
    run(UP_AT + 100u);
    TEST_ASSERT_EQUAL_UINT(1u, heard_func(ZW_FUNC_SEND_DATA, -1));
    TEST_ASSERT_EQUAL_UINT(1u, owner_done.n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, owner_done.status[0]);
}

/* ZW-004: the owner's requests may fill the queue but for its last slot, which is the host's (a
 * recovery's soft reset always finds room); the ones queued all go. */
static void test_owner_leaves_the_host_a_slot(void)
{
    script_send_data();
    start_host();
    run(UP_AT);
    for (uint32_t i = 1; i < ZW_TRANSPORT_QUEUE_LEN; i++) {
        TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_OK, submit_node_command(i, ZW_PRIO_BACKGROUND));
    }
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_FULL, submit_node_command(99, ZW_PRIO_BACKGROUND));
    run(UP_AT + 5000u);
    TEST_ASSERT_EQUAL_UINT(ZW_TRANSPORT_QUEUE_LEN - 1u, owner_done.n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, owner_done.status[ZW_TRANSPORT_QUEUE_LEN - 2u]);
}

/* ---- A jammed module (serial-api.md §6.12, §7) -------------------------------------------- */

/* ZW-004: the owner's recovery (zw_host_recover) - refused until the link is up; then the gate
 * closes, SoftReset, steps 13-19 again, the link up once more with no link down on the way. */
static void test_owner_asks_recovery(void)
{
    start_host();
    TEST_ASSERT_FALSE(zw_host_recover(&host, wire.now)); /* starting */
    run(UP_AT);
    TEST_ASSERT_EQUAL_UINT(1u, heard.ups);
    TEST_ASSERT_TRUE(zw_host_recover(&host, wire.now));
    TEST_ASSERT_EQUAL_INT(ZW_LINK_RECOVERING, zw_host_link(&host));
    TEST_ASSERT_FALSE(zw_host_recover(&host, wire.now)); /* one at a time */
    run(UP_AT + 1000u);
    TEST_ASSERT_EQUAL_UINT(2u, heard.ups);
    TEST_ASSERT_EQUAL_UINT(0u, heard.downs);
    TEST_ASSERT_EQUAL_UINT32(1u, zw_host_stats(&host)->recoveries);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(&host));
}

static fake_zdev_t jam_dev;
static const uint8_t JAM_NOOP[] = {0x00};
static bool jam_power; /* the module lists the 16-bit power read (§6.3 API Setup) */

/* A panel (net_harness) with one listening S2 device, interviewed and quiet. */
static uint16_t jam_world(bool watch)
{
    h_init();
    if (jam_power) {
        const unsigned sub = ZW_SETUP_GET_POWERLEVEL_16BIT;
        H.zac.id.setup_supported[sub / 8u] =
            (uint8_t)(H.zac.id.setup_supported[sub / 8u] | 1u << (sub % 8u));
    }
    if (watch) {
        H.cfg.jam = true; /* §9 watching on: the jam trouble */
    }
    if (watch || jam_power) {
        h_boot(); /* the start-up again, with them */
    }
    h_device(&jam_dev, 0x4A414Du, false);
    fake_zdev_listening(&jam_dev);
    const uint16_t node = h_include(&jam_dev);
    TEST_ASSERT_NOT_EQUAL(0u, node);
    TEST_ASSERT_TRUE(h_until_ready(node, 120000u));
    h_run_for(10000u);
    H.done_n = 0u;
    return node;
}

/* A No Operation to the node, then the run until its end is heard (at most `ms`): when. */
static uint64_t jam_send(uint16_t node, uint32_t tag, uint64_t ms)
{
    const size_t n = H.done_n + 1u;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_net_send(&H.net, H.wire.now, node, ZW_SEC_AUTO, JAM_NOOP,
                                                sizeof JAM_NOOP, tag));
    const uint64_t end = H.wire.now + ms;
    while (H.done_n < n && H.wire.now < end) {
        h_run_for(10u);
    }
    TEST_ASSERT_EQUAL_UINT(n, H.done_n);
    TEST_ASSERT_EQUAL_UINT32(tag, H.done_tag[n - 1u]);
    return H.wire.now;
}

/* ZW-037: jammed twice (Fail, 0 transmit ticks), then sent: delivered about 2 s later - each
 * jammed transmission waited 1 s and went again as it was (the same bytes: the same sequence
 * number and nonce, but for the callback ID), no failure, no reset. */
static void test_jammed_twice_then_sent(void)
{
    const uint16_t node = jam_world(false);
    const uint32_t recoveries = zw_host_stats(&H.net.host)->recoveries;
    const unsigned ups = H.ups;
    const size_t from = H.zac.heard;
    H.zac.jam_sends = 2u;
    const uint64_t t0 = H.wire.now;
    const uint64_t at = jam_send(node, 0x4A01u, 10000u);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, H.done_r[0]);
    TEST_ASSERT_EQUAL_UINT32(2u, H.zac.jammed_sends);
    TEST_ASSERT_TRUE(at >= t0 + 2u * ZW_S2_JAM_WAIT_MS);
    TEST_ASSERT_TRUE(at < t0 + 2u * ZW_S2_JAM_WAIT_MS + 1000u);
    TEST_ASSERT_EQUAL_UINT32(recoveries, zw_host_stats(&H.net.host)->recoveries);
    TEST_ASSERT_EQUAL_UINT32(0u, H.net.stats.jam_resets);
    TEST_ASSERT_EQUAL_UINT(ups, H.ups);
    /* the three transmissions of the first frame: byte for byte the same, but the callback ID */
    const fake_heard_t *first = NULL;
    unsigned same = 0u;
    for (size_t i = from; i < H.zac.heard; i++) {
        const fake_heard_t *f = fake_zac93_heard(&H.zac, i);
        if (f == NULL || f->type != ZW_TYPE_REQUEST || f->func != (uint8_t)ZW_FUNC_SEND_DATA ||
            f->payload_len < 3u || f->payload[0] != (uint8_t)node) {
            continue;
        }
        if (first == NULL) {
            first = f;
        }
        if (f->payload_len == first->payload_len &&
            memcmp(f->payload, first->payload, f->payload_len - 1u) == 0) {
            same++;
        }
    }
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_TRUE(same >= 3u);
}

/* ZW-037, ZW-004: jammed 5 times (1 s apart): the module is soft-reset once (§7: steps 13-19
 * again, no link down), and the same frame goes once more, behind the gate - delivered. */
static void test_jammed_five_times_reset_then_sent(void)
{
    const uint16_t node = jam_world(false);
    const uint32_t recoveries = zw_host_stats(&H.net.host)->recoveries;
    const unsigned ups = H.ups;
    H.zac.jam_sends = ZW_S2_JAM_TRIES;
    const uint64_t t0 = H.wire.now;
    const uint64_t at = jam_send(node, 0x4A02u, 20000u);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, H.done_r[0]);
    TEST_ASSERT_EQUAL_UINT32(ZW_S2_JAM_TRIES, H.zac.jammed_sends);
    TEST_ASSERT_TRUE(at >= t0 + (ZW_S2_JAM_TRIES - 1u) * ZW_S2_JAM_WAIT_MS);
    TEST_ASSERT_EQUAL_UINT32(recoveries + 1u, zw_host_stats(&H.net.host)->recoveries);
    TEST_ASSERT_EQUAL_UINT32(1u, H.net.stats.jam_resets);
    TEST_ASSERT_EQUAL_UINT(ups + 1u, H.ups); /* the link up again after the soft reset */
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(&H.net.host));
}

/* ZW-037, ZW-004, ZW-024: the jam's soft reset restarts the module, which forgets SmartStart
 * listen mode and applies any power level set: once the link is up again the panel sets the mode
 * again (AddNode 49), and the power facts read before are no longer taken as known. */
static void test_jam_reset_sets_smartstart_again(void)
{
    jam_power = true;
    const uint16_t node = jam_world(false);
    jam_power = false;
    zw_net_smartstart(&H.net, H.wire.now, true);
    h_run_for(500u);
    TEST_ASSERT_TRUE(H.zac.smart_listen);
    TEST_ASSERT_TRUE(zw_net_smartstart_listening(&H.net));
    TEST_ASSERT_TRUE(zw_net_power_read(&H.net, H.wire.now));
    h_run_for(500u);
    TEST_ASSERT_TRUE(zw_net_power(&H.net)->known);
    const uint32_t adds = H.zac.by_func[ZW_FUNC_ADD_NODE_TO_NETWORK];
    H.zac.jam_sends = ZW_S2_JAM_TRIES;
    jam_send(node, 0x4A09u, 20000u);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, H.done_r[0]);
    TEST_ASSERT_EQUAL_UINT32(1u, H.net.stats.jam_resets);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(&H.net.host));
    h_run_for(500u);
    TEST_ASSERT_TRUE(H.zac.smart_listen); /* set again after the restart */
    TEST_ASSERT_TRUE(zw_net_smartstart_listening(&H.net));
    TEST_ASSERT_TRUE(H.zac.by_func[ZW_FUNC_ADD_NODE_TO_NETWORK] > adds);
    TEST_ASSERT_FALSE(zw_net_power(&H.net)->known);
}

/* ZW-037, ZW-004: still jammed after the reset: FAILED, the jam trouble stays (§9), and a second
 * jammed command in the same episode fails at once - no second reset. A quiet air ends the
 * episode (the trouble clears): the next jam may reset again. */
static void test_still_jammed_after_reset_fails(void)
{
    const uint16_t node = jam_world(true);
    const uint32_t recoveries = zw_host_stats(&H.net.host)->recoveries;
    H.zac.jam_sends = ZW_S2_JAM_TRIES + 1u;
    jam_send(node, 0x4A03u, 20000u);
    TEST_ASSERT_EQUAL_INT(ZW_S2_FAILED, H.done_r[0]);
    TEST_ASSERT_EQUAL_UINT32(ZW_S2_JAM_TRIES + 1u, H.zac.jammed_sends);
    TEST_ASSERT_EQUAL_UINT32(recoveries + 1u, zw_host_stats(&H.net.host)->recoveries);
    TEST_ASSERT_TRUE(zw_net_jammed(&H.net));
    /* the same episode: jammed once more, it fails at once, no reset */
    H.zac.jam_sends = 1u;
    const uint64_t t1 = H.wire.now;
    const uint64_t at = jam_send(node, 0x4A04u, 20000u);
    TEST_ASSERT_EQUAL_INT(ZW_S2_FAILED, H.done_r[1]);
    TEST_ASSERT_TRUE(at < t1 + ZW_S2_JAM_WAIT_MS);
    TEST_ASSERT_EQUAL_UINT32(ZW_S2_JAM_TRIES + 2u, H.zac.jammed_sends);
    TEST_ASSERT_EQUAL_UINT32(recoveries + 1u, zw_host_stats(&H.net.host)->recoveries);
    TEST_ASSERT_EQUAL_UINT32(1u, H.net.stats.jam_resets);
    /* the air clear again (no failures for ZW_JAM_HOLD_MS, quiet readings): the episode is over */
    h_run_for(ZW_JAM_HOLD_MS + 10000u);
    TEST_ASSERT_FALSE(zw_net_jammed(&H.net));
    H.zac.jam_sends = ZW_S2_JAM_TRIES;
    jam_send(node, 0x4A05u, 20000u);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, H.done_r[2]);
    TEST_ASSERT_EQUAL_UINT32(recoveries + 2u, zw_host_stats(&H.net.host)->recoveries);
    TEST_ASSERT_EQUAL_UINT32(2u, H.net.stats.jam_resets);
}

/* ZW-037, ZW-004: a delivery ends the episode (jam watching off): after a failure past the
 * episode's reset, one frame delivered, and the next 5 jams reset the module again. */
static void test_delivery_ends_jam_episode(void)
{
    const uint16_t node = jam_world(false);
    const uint32_t recoveries = zw_host_stats(&H.net.host)->recoveries;
    H.zac.jam_sends = ZW_S2_JAM_TRIES + 1u;
    jam_send(node, 0x4A06u, 20000u);
    TEST_ASSERT_EQUAL_INT(ZW_S2_FAILED, H.done_r[0]);
    TEST_ASSERT_EQUAL_UINT32(recoveries + 1u, zw_host_stats(&H.net.host)->recoveries);
    jam_send(node, 0x4A07u, 5000u); /* the air is back */
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, H.done_r[1]);
    H.zac.jam_sends = ZW_S2_JAM_TRIES;
    jam_send(node, 0x4A08u, 20000u);
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, H.done_r[2]);
    TEST_ASSERT_EQUAL_UINT32(recoveries + 2u, zw_host_stats(&H.net.host)->recoveries);
    TEST_ASSERT_EQUAL_UINT32(2u, H.net.stats.jam_resets);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_recorded_startup_exchange);
    RUN_TEST(test_facts_at_link_up);
    RUN_TEST(test_refuses_end_node_then_swapped);
    RUN_TEST(test_refuses_wrong_chip);
    RUN_TEST(test_refuses_missing_function);
    RUN_TEST(test_refuses_wrong_library_accepts_bridge);
    RUN_TEST(test_old_protocol_note);
    RUN_TEST(test_region_differs);
    RUN_TEST(test_region_same);
    RUN_TEST(test_long_range_start);
    RUN_TEST(test_long_range_wanted_region_without);
    RUN_TEST(test_region_refused);
    RUN_TEST(test_node_id_type_refused);
    RUN_TEST(test_suc_left_alone);
    RUN_TEST(test_step_fails_twice_then_passes);
    RUN_TEST(test_step_fails_three_times_recovers);
    RUN_TEST(test_step_keeps_failing_link_down);
    RUN_TEST(test_bad_answer_link_down);
    RUN_TEST(test_reset_line_revives_hung_module_at_start);
    RUN_TEST(test_hang_while_up_recovered_by_reset_line);
    RUN_TEST(test_silent_without_reset_line_goes_down);
    RUN_TEST(test_dead_module_reset_line_then_down);
    RUN_TEST(test_retry_schedule);
    RUN_TEST(test_unexpected_restart_while_up);
    RUN_TEST(test_restart_while_starting);
    RUN_TEST(test_restart_before_step_12);
    RUN_TEST(test_start_again_mid_step);
    RUN_TEST(test_restarting_again_and_again);
    RUN_TEST(test_bootloader_run);
    RUN_TEST(test_bootloader_stuck);
    RUN_TEST(test_bootloader_run_never_starts);
    RUN_TEST(test_bootloader_banner_in_pieces);
    RUN_TEST(test_bootloader_while_up_recognised_at_once);
    RUN_TEST(test_bootloader_while_up_stuck_goes_down);
    RUN_TEST(test_link_down_fails_queued_requests);
    RUN_TEST(test_requests_wait_while_starting);
    RUN_TEST(test_owner_leaves_the_host_a_slot);
    RUN_TEST(test_owner_asks_recovery);
    RUN_TEST(test_jammed_twice_then_sent);
    RUN_TEST(test_jammed_five_times_reset_then_sent);
    RUN_TEST(test_jam_reset_sets_smartstart_again);
    RUN_TEST(test_still_jammed_after_reset_fails);
    RUN_TEST(test_delivery_ends_jam_episode);
    return UNITY_END();
}
