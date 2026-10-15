/*
 * test_zw_transport.c - the Serial API transport and request scheduler (components/core/
 * zwave_host, docs/features/36 serial-api.md §1 "Sending", §2, §6.8, §6.12 and §7) against the
 * fake ZAC93 on a fake clock: every rule of §1's table and state diagram, SendData's "not
 * queued", abort and give-up times, SoftReset and an unexpected restart, module frames that
 * arrive meanwhile, the scheduler's priorities, capacity and cancelling, and what a completion
 * callback may do. The times are the spec's, written out as numbers here on purpose.
 *
 * Requirements: ZW-003 (and ZW-004 where the SoftReset's damaged ACK meets the bootloader's text)
 */
#include <string.h>

#include "fake_zac93.h"
#include "unity.h"
#include "zwave_host/zw_transport.h"
#include "zwave_proto/zw_api.h"

/* The Serial API functions used here (serial-api.md §6), short names for zw_func_t's. */
#define F_APP_NODE_INFO  ZW_FUNC_SET_APPLICATION_NODE_INFORMATION /* ACK only */
#define F_APP_COMMAND    ZW_FUNC_APPLICATION_COMMAND_HANDLER      /* from the module */
#define F_SOFT_RESET     ZW_FUNC_SOFT_RESET
#define F_STARTED        ZW_FUNC_SERIAL_API_STARTED               /* from the module */
#define F_SEND_DATA      ZW_FUNC_SEND_DATA
#define F_CONTROLLER_VER ZW_FUNC_GET_CONTROLLER_VERSION           /* a response */
#define F_ABORT          ZW_FUNC_SEND_DATA_ABORT
#define F_NEIGHBOURS     ZW_FUNC_REQUEST_NODE_NEIGHBOR_UPDATE     /* callbacks only */
#define F_ADD_NODE       ZW_FUNC_ADD_NODE_TO_NETWORK              /* status reports */
#define F_SUC_ROUTE      ZW_FUNC_ASSIGN_SUC_RETURN_ROUTE          /* a response and a callback */

static zw_transport_t tr;
static fake_zac93_t zac;
static fake_wire_t wire;
static unsigned fail_writes;

/* ---- What the owner heard ----------------------------------------------------------------- */

typedef struct {
    zw_tx_outcome_t status;
    uint32_t tag;
    uint8_t func, callback_id, transmissions;
    uint64_t at;
    bool has_response, has_callback;
    size_t response_len, callback_len;
    uint8_t response[64], callback[64];
} done_rec_t;

static done_rec_t done_log[320];
static size_t done_n;

static struct {
    zw_transport_event_t ev;
    uint64_t at;
    uint8_t func, reason;
} ev_log[16];
static size_t ev_n;

static struct {
    uint64_t at;
    uint8_t type, func;
    size_t len;
    uint8_t payload[64];
} frame_log[32];
static size_t frame_n;

static uint8_t text_log[256];
static size_t text_n;

static void record_done(uint32_t tag, const zw_tx_result_t *r)
{
    TEST_ASSERT_TRUE(done_n < sizeof done_log / sizeof done_log[0]);
    done_rec_t *d = &done_log[done_n++];
    memset(d, 0, sizeof *d);
    d->status = r->status;
    d->tag = tag;
    d->func = r->func;
    d->callback_id = r->callback_id;
    d->transmissions = r->transmissions;
    d->at = wire.now;
    d->has_response = r->response != NULL;
    d->response_len = r->response_len;
    if (r->response != NULL && r->response_len > 0) {
        memcpy(d->response, r->response, r->response_len < 64 ? r->response_len : 64);
    }
    d->has_callback = r->callback != NULL;
    d->callback_len = r->callback_len;
    if (r->callback != NULL && r->callback_len > 0) {
        memcpy(d->callback, r->callback, r->callback_len < 64 ? r->callback_len : 64);
    }
}

static void on_done(void *ctx, uint32_t tag, const zw_tx_result_t *r)
{
    (void)ctx;
    record_done(tag, r);
}

static bool port_write(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    if (fail_writes > 0) {
        fail_writes--;
        return false;
    }
    return fake_wire_write(&wire, bytes, len);
}

static void port_frame(void *ctx, const zw_frame_t *f)
{
    (void)ctx;
    TEST_ASSERT_TRUE(frame_n < sizeof frame_log / sizeof frame_log[0]);
    frame_log[frame_n].at = wire.now;
    frame_log[frame_n].type = f->type;
    frame_log[frame_n].func = f->func;
    frame_log[frame_n].len = f->payload_len;
    if (f->payload_len > 0) {
        memcpy(frame_log[frame_n].payload, f->payload, f->payload_len < 64 ? f->payload_len : 64);
    }
    frame_n++;
}

static void port_text(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    TEST_ASSERT_TRUE(text_n + len <= sizeof text_log);
    memcpy(&text_log[text_n], bytes, len);
    text_n += len;
}

static void port_event(void *ctx, zw_transport_event_t ev, const zw_frame_t *f)
{
    (void)ctx;
    TEST_ASSERT_TRUE(ev_n < sizeof ev_log / sizeof ev_log[0]);
    ev_log[ev_n].ev = ev;
    ev_log[ev_n].at = wire.now;
    ev_log[ev_n].func = f != NULL ? f->func : 0;
    ev_log[ev_n].reason = f != NULL && f->payload_len > 0 ? f->payload[0] : 0xFF;
    ev_n++;
}

/* The wire's view of the transport: zw_link's news and the zwave task's clock. */
static void host_control(void *ctx, uint64_t now, uint8_t b)
{
    zw_transport_on_control(ctx, now, b);
}

static void host_frame(void *ctx, uint64_t now, const zw_frame_t *f)
{
    zw_transport_on_frame(ctx, now, f);
}

static void host_text(void *ctx, uint64_t now, const uint8_t *b, size_t n)
{
    zw_transport_on_text(ctx, now, b, n);
}

static void host_poll(void *ctx, uint64_t now)
{
    zw_transport_poll(ctx, now);
}

static uint64_t host_deadline(void *ctx)
{
    return zw_transport_next_deadline(ctx);
}

void setUp(void)
{
    const zw_transport_port_t port = {NULL, port_write, port_frame, port_text, port_event};
    const fake_host_t host = {&tr, host_control, host_frame, host_text, host_poll, host_deadline};
    zw_transport_init(&tr, &port);
    fake_zac93_init(&zac);
    zac.identity_answers = false; /* a bare module: only what each test scripts */
    fake_wire_init(&wire, &zac, &host, 0u);
    fail_writes = 0;
    done_n = ev_n = frame_n = text_n = 0;
}

void tearDown(void)
{
    TEST_ASSERT_FALSE_MESSAGE(wire.stuck, "the wire did not settle");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, zac.overflow, "the fake module ran out of room");
}

/* ---- Helpers ------------------------------------------------------------------------------ */

static zw_request_t request(uint8_t func, const uint8_t *payload, size_t len, uint8_t expect,
                            zw_prio_t prio, uint32_t tag)
{
    zw_request_t r;
    memset(&r, 0, sizeof r);
    r.func = func;
    r.payload = payload;
    r.payload_len = len;
    r.expect = expect;
    r.callback_id_at = len > 0 ? (uint8_t)(len - 1) : 0; /* the last byte, as usual */
    r.prio = prio;
    r.callback_timeout_ms = 5000;
    r.done = on_done;
    r.tag = tag;
    return r;
}

static void submit(const zw_request_t *r)
{
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_OK, zw_transport_submit(&tr, r));
}

static void set_answer(fake_answer_t *a, uint32_t after_ms, const uint8_t *bytes, size_t len)
{
    a->after_ms = after_ms;
    a->len = (uint8_t)len;
    memcpy(a->bytes, bytes, len);
}

/* A script: on func, a response (resp_len bytes, or none when resp is NULL) after resp_ms. */
static fake_rule_t rule_for(uint8_t func, const uint8_t *resp, size_t resp_len, uint32_t resp_ms)
{
    fake_rule_t r;
    memset(&r, 0, sizeof r);
    r.func = func;
    if (resp != NULL) {
        r.respond = true;
        set_answer(&r.response, resp_ms, resp, resp_len);
    }
    return r;
}

static void add_callback(fake_rule_t *r, uint32_t after_ms, const uint8_t *bytes, size_t len)
{
    set_answer(&r->callback[r->callbacks++], after_ms, bytes, len);
}

static void script(const fake_rule_t *r)
{
    TEST_ASSERT_TRUE(fake_zac93_on(&zac, r));
}

static const fake_heard_t *heard(size_t i)
{
    const fake_heard_t *h = fake_zac93_heard(&zac, i);
    TEST_ASSERT_NOT_NULL(h);
    return h;
}

static void run(uint64_t t) { fake_wire_run_until(&wire, t); }

/* "Z-Wave 7.22", NUL, library type 1 (static controller) - serial-api.md §6.4. */
static const uint8_t VERSION[] = {'Z', '-', 'W', 'a', 'v', 'e', ' ', '7', '.', '2', '2', 0x00,
                                  0x01};

/* SendData to node 5: Basic Set FF, transmit options 25, callback ID (serial-api.md §6.12). */
static const uint8_t SEND_BASIC_SET[] = {0x05, 0x03, 0x20, 0x01, 0xFF, 0x25, 0x00};

/* GetControllerVersion, answered 5 ms after its ACK. */
static void script_version(void)
{
    const fake_rule_t r = rule_for(F_CONTROLLER_VER, VERSION, sizeof VERSION, 5);
    script(&r);
}

static void submit_version(uint32_t tag)
{
    const zw_request_t r =
        request(F_CONTROLLER_VER, NULL, 0, ZW_EXPECT_RESPONSE, ZW_PRIO_MODULE, tag);
    submit(&r);
}

/* ---- A command's life (§1 "Sending", the state diagram) ------------------------------------- */

/* ZW-003: a command with a response - §1's GetControllerVersion frame 01 03 00 15 E9 on the
 * wire, ACK, then the response (TYPE 01, same FUNC) finishes it; nothing is sent before a poll,
 * and the deadline says "at once" while a request waits. */
static void test_command_with_response(void)
{
    static const uint8_t frame[] = {0x01, 0x03, 0x00, 0x15, 0xE9};
    script_version();
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, zw_transport_next_deadline(&tr));
    submit_version(7);
    TEST_ASSERT_EQUAL_UINT(0u, zac.heard); /* submit only queues */
    TEST_ASSERT_EQUAL_UINT64(0u, zw_transport_next_deadline(&tr));
    run(1000);
    TEST_ASSERT_EQUAL_UINT(1u, zac.heard);
    TEST_ASSERT_EQUAL_UINT(sizeof frame, heard(0)->raw_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(frame, heard(0)->raw, sizeof frame);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT32(7u, done_log[0].tag);
    TEST_ASSERT_EQUAL_UINT64(5u, done_log[0].at);
    TEST_ASSERT_EQUAL_UINT8(1u, done_log[0].transmissions);
    TEST_ASSERT_EQUAL_UINT8(0u, done_log[0].callback_id);
    TEST_ASSERT_TRUE(done_log[0].has_response);
    TEST_ASSERT_EQUAL_UINT(sizeof VERSION, done_log[0].response_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(VERSION, done_log[0].response, sizeof VERSION);
    TEST_ASSERT_FALSE(done_log[0].has_callback);
    TEST_ASSERT_FALSE(zw_transport_busy(&tr));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, zw_transport_next_deadline(&tr));
    TEST_ASSERT_EQUAL_UINT(0u, frame_n);
}

/* ZW-003: a command with a callback only - ACK, then TYPE 00 with its FUNC and its callback ID
 * as payload byte 0; the transport wrote the ID it allocated into the payload. */
static void test_command_with_callback(void)
{
    static const uint8_t done_status[] = {0x22};
    const uint8_t payload[] = {0x05, 0x00}; /* node 5, callback ID */
    fake_rule_t r = rule_for(F_NEIGHBOURS, NULL, 0, 0);
    add_callback(&r, 300, done_status, sizeof done_status);
    script(&r);
    const zw_request_t q = request(F_NEIGHBOURS, payload, sizeof payload, ZW_EXPECT_CALLBACK,
                                   ZW_PRIO_NODE, 1);
    submit(&q);
    run(299);
    TEST_ASSERT_EQUAL_UINT(0u, done_n);
    TEST_ASSERT_EQUAL_UINT64(5000u, zw_transport_next_deadline(&tr)); /* the callback timeout */
    run(1000);
    TEST_ASSERT_EQUAL_UINT8(0x01u, heard(0)->payload[1]);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT64(300u, done_log[0].at);
    TEST_ASSERT_EQUAL_UINT8(0x01u, done_log[0].callback_id);
    TEST_ASSERT_FALSE(done_log[0].has_response);
    TEST_ASSERT_EQUAL_UINT(2u, done_log[0].callback_len);
    TEST_ASSERT_EQUAL_HEX8(0x01u, done_log[0].callback[0]);
    TEST_ASSERT_EQUAL_HEX8(0x22u, done_log[0].callback[1]);
}

/* ZW-003: a command with both - ACK, the response, then the callback. The response timeout
 * (10 s) runs from the ACK, the callback timeout from the response. */
static void test_command_with_response_and_callback(void)
{
    static const uint8_t started[] = {0x01}, status_ok[] = {0x00};
    const uint8_t payload[] = {0x05, 0x00};
    fake_rule_t r = rule_for(F_SUC_ROUTE, started, sizeof started, 10);
    add_callback(&r, 500, status_ok, sizeof status_ok);
    script(&r);
    const zw_request_t q = request(F_SUC_ROUTE, payload, sizeof payload,
                                   ZW_EXPECT_RESPONSE | ZW_EXPECT_CALLBACK, ZW_PRIO_NODE, 1);
    submit(&q);
    run(0);
    TEST_ASSERT_EQUAL_UINT64(10000u, zw_transport_next_deadline(&tr));
    run(10);
    TEST_ASSERT_EQUAL_UINT64(5010u, zw_transport_next_deadline(&tr));
    run(1000);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT64(500u, done_log[0].at);
    TEST_ASSERT_EQUAL_HEX8(0x01u, done_log[0].response[0]);
    TEST_ASSERT_EQUAL_UINT(2u, done_log[0].callback_len);
    TEST_ASSERT_EQUAL_HEX8(0x00u, done_log[0].callback[1]);
}

/* ZW-003: callback IDs 1..255, wrapping, never 0 - 300 commands in a row, each queued by the
 * previous one's done() (which may submit). */
static unsigned chain_left;
static const uint8_t CHAIN_PAYLOAD[] = {0x05, 0x00};

static void chain_done(void *ctx, uint32_t tag, const zw_tx_result_t *r)
{
    (void)ctx;
    record_done(tag, r);
    if (chain_left > 0) {
        chain_left--;
        zw_request_t q = request(F_NEIGHBOURS, CHAIN_PAYLOAD, sizeof CHAIN_PAYLOAD,
                                 ZW_EXPECT_CALLBACK, ZW_PRIO_NODE, tag + 1);
        q.done = chain_done;
        TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_OK, zw_transport_submit(&tr, &q));
    }
}

static void test_callback_ids_wrap_and_skip_zero(void)
{
    static const uint8_t status_done[] = {0x22};
    fake_rule_t r = rule_for(F_NEIGHBOURS, NULL, 0, 0);
    add_callback(&r, 1, status_done, sizeof status_done);
    script(&r);
    chain_left = 299;
    zw_request_t q = request(F_NEIGHBOURS, CHAIN_PAYLOAD, sizeof CHAIN_PAYLOAD,
                             ZW_EXPECT_CALLBACK, ZW_PRIO_NODE, 0);
    q.done = chain_done;
    submit(&q);
    run(10000);
    TEST_ASSERT_EQUAL_UINT(300u, done_n);
    for (size_t k = 0; k < 300; k++) {
        TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[k].status);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(k % 255 + 1), done_log[k].callback_id);
        TEST_ASSERT_EQUAL_HEX8(done_log[k].callback_id, done_log[k].callback[0]);
    }
    for (size_t k = 300 - FAKE_ZAC93_LOG; k < 300; k++) { /* and on the wire */
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(k % 255 + 1), heard(k)->payload[1]);
    }
}

/* ZW-003: NAK -> the 2nd transmission 100 ms later, a 2nd NAK -> the 3rd 1100 ms after that, a
 * 3rd NAK -> failed, and the module is not answering (§7); no 4th transmission. */
static void test_nak_retries_then_not_answering(void)
{
    script_version();
    zac.fault.nak_next = 3;
    submit_version(1);
    run(99);
    TEST_ASSERT_EQUAL_UINT(1u, zac.heard);
    run(100);
    TEST_ASSERT_EQUAL_UINT(2u, zac.heard);
    run(1199);
    TEST_ASSERT_EQUAL_UINT(2u, zac.heard);
    run(1200);
    TEST_ASSERT_EQUAL_UINT(3u, zac.heard);
    for (size_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT(FAKE_NAKED, heard(i)->answered);
    }
    TEST_ASSERT_EQUAL_UINT64(0u, heard(0)->at);
    TEST_ASSERT_EQUAL_UINT64(100u, heard(1)->at);
    TEST_ASSERT_EQUAL_UINT64(1200u, heard(2)->at);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_NO_ACK, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT8(3u, done_log[0].transmissions);
    TEST_ASSERT_EQUAL_UINT64(1200u, done_log[0].at);
    TEST_ASSERT_EQUAL_UINT(1u, ev_n);
    TEST_ASSERT_EQUAL_INT(ZW_TRANSPORT_NOT_ANSWERING, ev_log[0].ev);
    TEST_ASSERT_EQUAL_UINT64(1200u, ev_log[0].at);
    run(20000);
    TEST_ASSERT_EQUAL_UINT(3u, zac.heard);
    TEST_ASSERT_EQUAL_UINT32(3u, tr.stats.naks);
}

/* ZW-003: two NAKs, then the 3rd transmission is heard and the command succeeds. */
static void test_nak_twice_then_ok(void)
{
    script_version();
    zac.fault.nak_next = 2;
    submit_version(1);
    run(5000);
    TEST_ASSERT_EQUAL_UINT(3u, zac.heard);
    TEST_ASSERT_EQUAL_INT(FAKE_ACKED, heard(2)->answered);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT8(3u, done_log[0].transmissions);
    TEST_ASSERT_EQUAL_UINT64(1205u, done_log[0].at);
    TEST_ASSERT_EQUAL_UINT(0u, ev_n);
}

/* ZW-003: the ACK timeout is 1600 ms after the frame is written - nothing at 1599, the timeout
 * at 1600, the 2nd transmission 100 ms later. */
static void test_ack_timeout_at_1600(void)
{
    const uint8_t nif[] = {0x01, 0x02, 0x01, 0x00};
    const zw_request_t q = request(F_APP_NODE_INFO, nif, sizeof nif, 0, ZW_PRIO_MODULE, 1);
    zac.fault.drop_next_ack = true;
    submit(&q);
    run(0);
    TEST_ASSERT_EQUAL_UINT64(1600u, zw_transport_next_deadline(&tr));
    run(1599);
    TEST_ASSERT_EQUAL_UINT32(0u, tr.stats.ack_timeouts);
    TEST_ASSERT_EQUAL_UINT(1u, zac.heard);
    run(1600);
    TEST_ASSERT_EQUAL_UINT32(1u, tr.stats.ack_timeouts);
    TEST_ASSERT_EQUAL_UINT64(1700u, zw_transport_next_deadline(&tr));
    run(1699);
    TEST_ASSERT_EQUAL_UINT(1u, zac.heard);
    run(1700);
    TEST_ASSERT_EQUAL_UINT(2u, zac.heard);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT64(1700u, done_log[0].at);
    TEST_ASSERT_EQUAL_UINT8(2u, done_log[0].transmissions);
}

/* ZW-003: a lost ACK (§1) - the response that arrives while the ACK is still awaited counts as
 * the ACK and the response; the command is done, and never sent again. */
static void test_response_before_ack_counts_as_ack(void)
{
    script_version();
    zac.fault.drop_next_ack = true;
    submit_version(1);
    run(5000);
    TEST_ASSERT_EQUAL_UINT(1u, zac.heard);
    TEST_ASSERT_EQUAL_INT(FAKE_ACK_LOST, heard(0)->answered);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT64(5u, done_log[0].at);
    TEST_ASSERT_EQUAL_UINT8(1u, done_log[0].transmissions);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(VERSION, done_log[0].response, sizeof VERSION);
    TEST_ASSERT_EQUAL_UINT(0u, frame_n);
    TEST_ASSERT_EQUAL_UINT32(1u, tr.stats.lost_acks);
    TEST_ASSERT_EQUAL_UINT32(0u, tr.stats.acks);
    TEST_ASSERT_EQUAL_UINT32(0u, tr.stats.ack_timeouts);
}

/* ZW-003: the same for SendData - its "queued" response stands in for the lost ACK: exactly one
 * transmission (a second would send to the node twice), the callback ends it, and the 20 s
 * abort clock runs from "queued". */
static void test_send_data_lost_ack_is_sent_once(void)
{
    static const uint8_t queued[] = {0x01}, tx_ok[] = {0x00, 0x00, 0x03};
    fake_rule_t r = rule_for(F_SEND_DATA, queued, sizeof queued, 10);
    add_callback(&r, 3000, tx_ok, sizeof tx_ok);
    script(&r);
    zac.fault.drop_next_ack = true;
    const zw_request_t q = request(F_SEND_DATA, SEND_BASIC_SET, sizeof SEND_BASIC_SET,
                                   ZW_EXPECT_RESPONSE | ZW_EXPECT_CALLBACK, ZW_PRIO_NODE, 1);
    submit(&q);
    run(10);
    TEST_ASSERT_EQUAL_UINT64(20010u, zw_transport_next_deadline(&tr)); /* SendDataAbort */
    run(40000);
    TEST_ASSERT_EQUAL_UINT(1u, zac.heard);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT64(3000u, done_log[0].at);
    TEST_ASSERT_EQUAL_UINT8(1u, done_log[0].transmissions);
    TEST_ASSERT_EQUAL_HEX8(0x01u, done_log[0].response[0]);
    TEST_ASSERT_EQUAL_UINT32(1u, tr.stats.lost_acks);
    TEST_ASSERT_EQUAL_UINT32(0u, tr.stats.aborts);
}

/* ZW-003: a command whose only answer is its callback: the callback stands in for a lost ACK. */
static void test_callback_before_ack_counts_as_ack(void)
{
    static const uint8_t done_status[] = {0x22};
    const uint8_t payload[] = {0x05, 0x00};
    fake_rule_t r = rule_for(F_NEIGHBOURS, NULL, 0, 0);
    add_callback(&r, 300, done_status, sizeof done_status);
    script(&r);
    zac.fault.drop_next_ack = true;
    const zw_request_t q = request(F_NEIGHBOURS, payload, sizeof payload, ZW_EXPECT_CALLBACK,
                                   ZW_PRIO_NODE, 1);
    submit(&q);
    run(5000);
    TEST_ASSERT_EQUAL_UINT(1u, zac.heard);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT64(300u, done_log[0].at);
    TEST_ASSERT_EQUAL_UINT8(1u, done_log[0].transmissions);
    TEST_ASSERT_EQUAL_UINT32(1u, tr.stats.lost_acks);
}

/* ZW-003: ...but not for a command that also waits for a response (the module sends that first):
 * a callback while its ACK is awaited is the module's frame, for the owner, and the command goes
 * again after its ACK timeout. */
static void test_callback_before_ack_of_command_with_response_not_taken(void)
{
    static const uint8_t tx_ok[] = {0x00, 0x00, 0x03};
    fake_rule_t r = rule_for(F_SEND_DATA, NULL, 0, 0);
    add_callback(&r, 300, tx_ok, sizeof tx_ok);
    script(&r);
    zac.fault.drop_next_ack = true;
    const zw_request_t q = request(F_SEND_DATA, SEND_BASIC_SET, sizeof SEND_BASIC_SET,
                                   ZW_EXPECT_RESPONSE | ZW_EXPECT_CALLBACK, ZW_PRIO_NODE, 1);
    submit(&q);
    run(300);
    TEST_ASSERT_EQUAL_UINT(0u, done_n);
    TEST_ASSERT_EQUAL_UINT(1u, frame_n);
    TEST_ASSERT_EQUAL_UINT32(0u, tr.stats.lost_acks);
    run(1699);
    TEST_ASSERT_EQUAL_UINT(1u, zac.heard);
    run(1700);
    TEST_ASSERT_EQUAL_UINT(2u, zac.heard);
}

/* ZW-003: an ACK after its 1600 ms is late: counted as stray, not taken; the frame still goes
 * again at 1700 ms. */
static void test_late_ack_is_stray(void)
{
    script_version();
    zac.fault.ack_delay_ms = 1650;
    submit_version(1);
    run(1650);
    TEST_ASSERT_EQUAL_UINT32(1u, tr.stats.stray);
    TEST_ASSERT_EQUAL_UINT(0u, done_n);
    run(5000);
    TEST_ASSERT_EQUAL_UINT(2u, zac.heard);
    TEST_ASSERT_EQUAL_UINT64(1700u, heard(1)->at);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT8(2u, done_log[0].transmissions);
}

/* ZW-003: CAN -> 100 ms, and the module's own frame (why it said CAN) is handled first; a CAN
 * on the 2nd transmission waits 100 ms again (not 1100). */
static void test_can_waits_100_ms_and_module_frame_first(void)
{
    static const uint8_t report[] = {0x00, 0x05, 0x03, 0x20, 0x03, 0xFF}; /* node 5: Basic Report */
    script_version();
    zac.fault.can_next = true;
    TEST_ASSERT_TRUE(fake_zac93_send_at(&zac, 0, ZW_TYPE_REQUEST, F_APP_COMMAND, report,
                                        sizeof report));
    submit_version(1);
    run(0);
    TEST_ASSERT_EQUAL_INT(FAKE_CANNED, heard(0)->answered);
    TEST_ASSERT_EQUAL_UINT(1u, frame_n); /* the module's frame, before any retransmission */
    TEST_ASSERT_EQUAL_HEX8(F_APP_COMMAND, frame_log[0].func);
    TEST_ASSERT_EQUAL_UINT64(100u, zw_transport_next_deadline(&tr));
    run(50);
    zac.fault.can_next = true; /* the 2nd transmission collides too */
    run(199);
    TEST_ASSERT_EQUAL_UINT(2u, zac.heard);
    TEST_ASSERT_EQUAL_UINT64(100u, heard(1)->at);
    TEST_ASSERT_EQUAL_INT(FAKE_CANNED, heard(1)->answered);
    run(1000);
    TEST_ASSERT_EQUAL_UINT(3u, zac.heard);
    TEST_ASSERT_EQUAL_UINT64(200u, heard(2)->at);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT32(2u, tr.stats.cans);
}

/* ZW-003: three CANs are three failed transmissions: failed, not answering. */
static void test_three_cans_fail(void)
{
    script_version();
    zac.fault.can_next = true;
    submit_version(1);
    run(50);
    zac.fault.can_next = true;
    run(150);
    zac.fault.can_next = true;
    run(1000);
    TEST_ASSERT_EQUAL_UINT(3u, zac.heard);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_NO_ACK, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT64(200u, done_log[0].at);
    TEST_ASSERT_EQUAL_UINT(1u, ev_n);
    TEST_ASSERT_EQUAL_INT(ZW_TRANSPORT_NOT_ANSWERING, ev_log[0].ev);
}

/* ZW-003: the response timeout - 10 s after the ACK (nothing at 9999), or the request's own;
 * a response after it is not taken. §7: a command ACKed whose response never comes means the
 * module is not answering. (A response carries no ID: one this late would be taken by a next
 * command with the same FUNC already waiting - the protocol cannot tell them apart - so here it
 * arrives while nothing waits.) */
static void test_response_timeout(void)
{
    static const uint8_t late[] = {0x01};
    submit_version(1); /* no script: ACK, and no response */
    run(9999);
    TEST_ASSERT_EQUAL_UINT(0u, done_n);
    TEST_ASSERT_EQUAL_UINT(0u, ev_n);
    run(10000);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_RESPONSE_TIMEOUT, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT64(10000u, done_log[0].at);
    TEST_ASSERT_FALSE(done_log[0].has_response);
    TEST_ASSERT_EQUAL_UINT(1u, ev_n);
    TEST_ASSERT_EQUAL_INT(ZW_TRANSPORT_NOT_ANSWERING, ev_log[0].ev);
    TEST_ASSERT_EQUAL_UINT64(10000u, ev_log[0].at);
    TEST_ASSERT_TRUE(fake_zac93_send_at(&zac, 10500, ZW_TYPE_RESPONSE, F_CONTROLLER_VER, late,
                                        sizeof late));
    run(11000);
    TEST_ASSERT_EQUAL_UINT(1u, frame_n); /* the late response, to the owner */
    zw_request_t q = request(F_CONTROLLER_VER, NULL, 0, ZW_EXPECT_RESPONSE, ZW_PRIO_MODULE, 2);
    q.response_timeout_ms = 2000;
    submit(&q);
    run(20000);
    TEST_ASSERT_EQUAL_UINT(2u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_RESPONSE_TIMEOUT, done_log[1].status);
    TEST_ASSERT_EQUAL_UINT64(13000u, done_log[1].at);
    TEST_ASSERT_EQUAL_UINT(2u, ev_n);
    TEST_ASSERT_EQUAL_INT(ZW_TRANSPORT_NOT_ANSWERING, ev_log[1].ev);
    TEST_ASSERT_EQUAL_UINT64(13000u, ev_log[1].at);
}

/* ZW-003: the callback timeout, the request's own: nothing at 4999, failed at 5000; a late
 * callback is handed to the owner, never matched. */
static void test_callback_timeout(void)
{
    static const uint8_t late[] = {0x01, 0x22};
    const uint8_t payload[] = {0x05, 0x00};
    const zw_request_t q = request(F_NEIGHBOURS, payload, sizeof payload, ZW_EXPECT_CALLBACK,
                                   ZW_PRIO_NODE, 1);
    /* the fake's own neighbour update (route repair) answers in time: only the late one here */
    zac.fault.mute_func = F_NEIGHBOURS;
    zac.fault.mute_count = FAKE_MUTE_FOREVER;
    submit(&q);
    TEST_ASSERT_TRUE(fake_zac93_send_at(&zac, 6000, ZW_TYPE_REQUEST, F_NEIGHBOURS, late,
                                        sizeof late));
    run(4999);
    TEST_ASSERT_EQUAL_UINT(0u, done_n);
    run(5000);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_CALLBACK_TIMEOUT, done_log[0].status);
    run(10000);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_UINT(1u, frame_n);
    TEST_ASSERT_EQUAL_HEX8(F_NEIGHBOURS, frame_log[0].func);
    TEST_ASSERT_EQUAL_UINT(0u, ev_n);
}

/* ZW-003: module frames while a command waits go to the owner and are never mistaken for its
 * response or callback: a callback with another ID and a response with another FUNC while it
 * waits for its response, an ApplicationCommandHandler, then another callback ID and a second
 * response while it waits for its callback. */
static void test_interleaved_module_frames(void)
{
    static const uint8_t started[] = {0x01}, status_ok[] = {0x00};
    static const uint8_t other_cb[] = {0x99, 0x00}, other_cb2[] = {0x77, 0x00};
    static const uint8_t report[] = {0x00, 0x05, 0x03, 0x20, 0x03, 0xFF};
    const uint8_t payload[] = {0x05, 0x00};
    fake_rule_t r = rule_for(F_SUC_ROUTE, started, sizeof started, 10);
    add_callback(&r, 20, status_ok, sizeof status_ok);
    script(&r);
    TEST_ASSERT_TRUE(fake_zac93_send_at(&zac, 5, ZW_TYPE_REQUEST, F_SUC_ROUTE, other_cb,
                                        sizeof other_cb));
    TEST_ASSERT_TRUE(fake_zac93_send_at(&zac, 6, ZW_TYPE_RESPONSE, F_CONTROLLER_VER, VERSION,
                                        sizeof VERSION));
    TEST_ASSERT_TRUE(fake_zac93_send_at(&zac, 7, ZW_TYPE_REQUEST, F_APP_COMMAND, report,
                                        sizeof report));
    TEST_ASSERT_TRUE(fake_zac93_send_at(&zac, 15, ZW_TYPE_REQUEST, F_SUC_ROUTE, other_cb2,
                                        sizeof other_cb2));
    TEST_ASSERT_TRUE(fake_zac93_send_at(&zac, 16, ZW_TYPE_RESPONSE, F_SUC_ROUTE, started,
                                        sizeof started));
    const zw_request_t q = request(F_SUC_ROUTE, payload, sizeof payload,
                                   ZW_EXPECT_RESPONSE | ZW_EXPECT_CALLBACK, ZW_PRIO_NODE, 1);
    submit(&q);
    run(1000);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT64(20u, done_log[0].at);
    TEST_ASSERT_EQUAL_HEX8(0x01u, done_log[0].callback[0]);
    TEST_ASSERT_EQUAL_UINT(5u, frame_n);
    const uint64_t at[] = {5, 6, 7, 15, 16};
    const uint8_t func[] = {F_SUC_ROUTE, F_CONTROLLER_VER, F_APP_COMMAND, F_SUC_ROUTE, F_SUC_ROUTE};
    for (size_t i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_UINT64(at[i], frame_log[i].at);
        TEST_ASSERT_EQUAL_HEX8(func[i], frame_log[i].func);
    }
    TEST_ASSERT_EQUAL_UINT32(5u, tr.stats.unclaimed);
}

/* RequestNodeNeighborUpdate's reports (serial-api.md §6.19): 21 started keeps it going, 22
 * done and 23 failed end it. */
static unsigned kinds_seen;

static zw_callback_kind_t neighbour_kind(void *ctx, const uint8_t *payload, size_t len)
{
    (void)ctx;
    kinds_seen++;
    return len >= 2 && payload[1] == 0x21 ? ZW_CALLBACK_INTERMEDIATE : ZW_CALLBACK_FINAL;
}

static zw_request_t neighbour_update(uint32_t callback_timeout_ms)
{
    static const uint8_t payload[] = {0x05, 0x00}; /* node 5, callback ID */
    zw_request_t q = request(F_NEIGHBOURS, payload, sizeof payload, ZW_EXPECT_CALLBACK,
                             ZW_PRIO_NODE, 1);
    q.callback_timeout_ms = callback_timeout_ms;
    q.callback_kind = neighbour_kind;
    return q;
}

/* ZW-003: an intermediate callback, then the final one - the classifier sees each, and the
 * intermediate one starts the callback timeout again (the diagram's WaitCallback self-loop):
 * "started" 2 s and "done" 2.5 s later finish under a 3 s timeout that 4.5 s in all exceeds. */
static void test_intermediate_then_final_callbacks(void)
{
    static const uint8_t started[] = {0x21}, done_status[] = {0x22};
    fake_rule_t r = rule_for(F_NEIGHBOURS, NULL, 0, 0);
    add_callback(&r, 2000, started, sizeof started);
    add_callback(&r, 4500, done_status, sizeof done_status);
    script(&r);
    const zw_request_t q = neighbour_update(3000);
    kinds_seen = 0;
    submit(&q);
    run(10000);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT64(4500u, done_log[0].at);
    TEST_ASSERT_EQUAL_UINT(2u, done_log[0].callback_len);
    TEST_ASSERT_EQUAL_HEX8(0x22u, done_log[0].callback[1]);
    TEST_ASSERT_EQUAL_UINT(2u, kinds_seen);
    TEST_ASSERT_EQUAL_UINT32(1u, tr.stats.intermediate);
}

/* ZW-003: ...but the timeout still holds between reports: "started" at 100 ms, then nothing
 * for 3 s -> failed at 3100; the final report after that goes to the owner. */
static void test_intermediate_callback_then_timeout(void)
{
    static const uint8_t started[] = {0x21}, done_status[] = {0x22};
    fake_rule_t r = rule_for(F_NEIGHBOURS, NULL, 0, 0);
    add_callback(&r, 100, started, sizeof started);
    add_callback(&r, 3200, done_status, sizeof done_status);
    script(&r);
    const zw_request_t q = neighbour_update(3000);
    submit(&q);
    run(10000);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_CALLBACK_TIMEOUT, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT64(3100u, done_log[0].at);
    TEST_ASSERT_EQUAL_UINT(1u, frame_n);
}

/* ZW-003: AddNodeToNetwork ends at its first status report, Ready (serial-api.md §1, §6.16):
 * the reports after it (NodeFound, AddingEndNode...) are the module's own frames, handed to the
 * owner for the inclusion logic. */
static void test_add_node_ends_at_ready(void)
{
    static const uint8_t ready[] = {0x01, 0x00, 0x00}, found[] = {0x02, 0x00, 0x00};
    const uint8_t payload[] = {0xC1, 0x00}; /* start: any, high power, network-wide */
    fake_rule_t r = rule_for(F_ADD_NODE, NULL, 0, 0);
    add_callback(&r, 50, ready, sizeof ready);
    add_callback(&r, 7000, found, sizeof found);
    script(&r);
    const zw_request_t q = request(F_ADD_NODE, payload, sizeof payload, ZW_EXPECT_CALLBACK,
                                   ZW_PRIO_MODULE, 1);
    submit(&q);
    run(10000);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT64(50u, done_log[0].at);
    TEST_ASSERT_EQUAL_HEX8(0x01u, done_log[0].callback[1]);
    TEST_ASSERT_EQUAL_UINT(1u, frame_n);
    TEST_ASSERT_EQUAL_UINT64(7000u, frame_log[0].at);
    TEST_ASSERT_EQUAL_HEX8(F_ADD_NODE, frame_log[0].func);
    TEST_ASSERT_EQUAL_HEX8(0x02u, frame_log[0].payload[1]);
}

/* ---- SendData (§6.12) --------------------------------------------------------------------- */

static void submit_send_data(uint32_t tag)
{
    const zw_request_t q = request(F_SEND_DATA, SEND_BASIC_SET, sizeof SEND_BASIC_SET,
                                   ZW_EXPECT_RESPONSE | ZW_EXPECT_CALLBACK, ZW_PRIO_NODE, tag);
    submit(&q);
}

/* ZW-003: SendData answered "not queued" (00) is sent again 500 ms later with the same
 * callback ID; the 3rd "not queued" fails it, and the module is not answering (§7). */
static void test_send_data_not_queued_three_times(void)
{
    static const uint8_t queued[] = {0x01};
    fake_rule_t r = rule_for(F_SEND_DATA, queued, sizeof queued, 0);
    r.not_queued = 3;
    script(&r);
    submit_send_data(1);
    run(999);
    TEST_ASSERT_EQUAL_UINT(2u, zac.heard);
    run(5000);
    TEST_ASSERT_EQUAL_UINT(3u, zac.heard);
    TEST_ASSERT_EQUAL_UINT64(0u, heard(0)->at);
    TEST_ASSERT_EQUAL_UINT64(500u, heard(1)->at);
    TEST_ASSERT_EQUAL_UINT64(1000u, heard(2)->at);
    for (size_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x01u, heard(i)->payload[6]);
    }
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_NOT_QUEUED, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT64(1000u, done_log[0].at);
    TEST_ASSERT_EQUAL_UINT8(3u, done_log[0].transmissions);
    TEST_ASSERT_EQUAL_HEX8(0x00u, done_log[0].response[0]);
    TEST_ASSERT_EQUAL_UINT(1u, ev_n);
    TEST_ASSERT_EQUAL_INT(ZW_TRANSPORT_NOT_ANSWERING, ev_log[0].ev);
    TEST_ASSERT_EQUAL_UINT32(3u, tr.stats.not_queued);
}

/* ZW-003: "not queued" twice, queued the 3rd time: the callback finishes it. */
static void test_send_data_not_queued_twice_then_queued(void)
{
    static const uint8_t queued[] = {0x01}, tx_ok[] = {0x00, 0x00, 0x03};
    fake_rule_t r = rule_for(F_SEND_DATA, queued, sizeof queued, 0);
    r.not_queued = 2;
    add_callback(&r, 40, tx_ok, sizeof tx_ok);
    script(&r);
    submit_send_data(1);
    run(5000);
    TEST_ASSERT_EQUAL_UINT(3u, zac.heard);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT64(1040u, done_log[0].at);
    TEST_ASSERT_EQUAL_HEX8(0x01u, done_log[0].response[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01u, done_log[0].callback[0]);
    TEST_ASSERT_EQUAL_UINT(0u, ev_n);
}

/* ZW-003: no SendData callback 20 s after "queued" -> SendDataAbort (01 03 00 16 EA), and the
 * wait goes on; none 30 s after -> failed, and the module is not answering (§7). */
static void test_send_data_abort_at_20s_fail_at_30s(void)
{
    static const uint8_t queued[] = {0x01};
    static const uint8_t abort_frame[] = {0x01, 0x03, 0x00, 0x16, 0xEA};
    const fake_rule_t r = rule_for(F_SEND_DATA, queued, sizeof queued, 0);
    script(&r);
    submit_send_data(1);
    run(0);
    TEST_ASSERT_EQUAL_UINT64(20000u, zw_transport_next_deadline(&tr));
    run(19999);
    TEST_ASSERT_EQUAL_UINT(1u, zac.heard);
    run(20000);
    TEST_ASSERT_EQUAL_UINT(2u, zac.heard);
    TEST_ASSERT_EQUAL_HEX8(F_ABORT, heard(1)->func);
    TEST_ASSERT_EQUAL_UINT(sizeof abort_frame, heard(1)->raw_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(abort_frame, heard(1)->raw, sizeof abort_frame);
    TEST_ASSERT_EQUAL_UINT64(30000u, zw_transport_next_deadline(&tr));
    run(29999);
    TEST_ASSERT_EQUAL_UINT(0u, done_n);
    run(30000);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_CALLBACK_TIMEOUT, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT(1u, ev_n);
    TEST_ASSERT_EQUAL_INT(ZW_TRANSPORT_NOT_ANSWERING, ev_log[0].ev);
    TEST_ASSERT_EQUAL_UINT64(30000u, ev_log[0].at);
    TEST_ASSERT_EQUAL_UINT32(1u, tr.stats.aborts);
    TEST_ASSERT_EQUAL_UINT32(0u, tr.stats.stray); /* the abort's ACK was expected */
}

/* ZW-003: the abort provokes the SendData callback (NoAck): the command ends with it. */
static void test_send_data_abort_provokes_callback(void)
{
    static const uint8_t queued[] = {0x01}, no_ack[] = {0x01};
    fake_rule_t r = rule_for(F_SEND_DATA, queued, sizeof queued, 0);
    r.callback_on_abort = true;
    set_answer(&r.abort_callback, 0, no_ack, sizeof no_ack);
    script(&r);
    submit_send_data(1);
    run(40000);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT64(20000u, done_log[0].at);
    TEST_ASSERT_EQUAL_UINT(2u, done_log[0].callback_len);
    TEST_ASSERT_EQUAL_HEX8(0x01u, done_log[0].callback[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01u, done_log[0].callback[1]);
    TEST_ASSERT_EQUAL_UINT(0u, ev_n);
    TEST_ASSERT_EQUAL_UINT32(0u, tr.stats.stray);
}

/* ZW-003: the next command waits for the abort's ACK (here lost: 1600 ms), so it cannot take
 * that ACK for its own. */
static void test_abort_ack_holds_the_next_command(void)
{
    static const uint8_t queued[] = {0x01}, no_ack[] = {0x01};
    fake_rule_t r = rule_for(F_SEND_DATA, queued, sizeof queued, 0);
    r.callback_on_abort = true;
    set_answer(&r.abort_callback, 0, no_ack, sizeof no_ack);
    script(&r);
    script_version();
    submit_send_data(1);
    run(0);
    submit_version(2); /* queued behind the SendData in flight */
    run(10000);
    zac.fault.drop_next_ack = true; /* the abort's */
    run(21599);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_UINT64(20000u, done_log[0].at);
    TEST_ASSERT_EQUAL_UINT(2u, zac.heard);
    run(21600);
    TEST_ASSERT_EQUAL_UINT(3u, zac.heard);
    TEST_ASSERT_EQUAL_HEX8(F_CONTROLLER_VER, heard(2)->func);
    run(30000);
    TEST_ASSERT_EQUAL_UINT(2u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[1].status);
    TEST_ASSERT_EQUAL_UINT8(1u, done_log[1].transmissions);
}

/* ---- SendData whose "queued" response is lost (§7, code review ZW12) ----------------------- */

/* SendData ACKed at 0 whose "queued" response never comes: its callback (cb_ms after the
 * frame, status first; none when cb_len is 0) is all that may still end it. */
static void script_send_data_without_response(uint32_t cb_ms, const uint8_t *cb, size_t cb_len)
{
    fake_rule_t r = rule_for(F_SEND_DATA, NULL, 0, 0);
    if (cb_len > 0) {
        add_callback(&r, cb_ms, cb, cb_len);
    }
    script(&r);
}

/* ZW-003 (§7, ZW12): the response lost, the callback OK 11 s after the ACK: the command ends OK
 * with it, the module is not reported as not answering, nothing is sent again or aborted, and
 * the command queued behind it goes next. Z-Wave JS's SerialAPICommandMachine keeps waiting for
 * the callback in the same way. */
static void test_send_data_lost_response_then_ok_callback(void)
{
    static const uint8_t tx_ok[] = {0x00, 0x00, 0x03};
    script_send_data_without_response(11000, tx_ok, sizeof tx_ok);
    script_version();
    submit_send_data(1);
    run(0);
    submit_version(2);
    run(10000); /* the response's 10 s are over: the callback is still awaited */
    TEST_ASSERT_EQUAL_UINT(0u, done_n);
    TEST_ASSERT_EQUAL_UINT(0u, ev_n);
    TEST_ASSERT_TRUE(zw_transport_busy(&tr));
    TEST_ASSERT_EQUAL_UINT64(20000u, zw_transport_next_deadline(&tr)); /* abort: ACK + 20 s */
    run(15000);
    TEST_ASSERT_EQUAL_UINT(2u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT32(1u, done_log[0].tag);
    TEST_ASSERT_EQUAL_UINT64(11000u, done_log[0].at);
    TEST_ASSERT_FALSE(done_log[0].has_response);
    TEST_ASSERT_EQUAL_UINT8(1u, done_log[0].transmissions); /* never sent again */
    TEST_ASSERT_EQUAL_UINT(4u, done_log[0].callback_len);
    TEST_ASSERT_EQUAL_HEX8(0x01u, done_log[0].callback[0]); /* its own callback ID */
    TEST_ASSERT_EQUAL_HEX8(0x00u, done_log[0].callback[1]); /* transmit status OK */
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[1].status);  /* the command queued behind it */
    TEST_ASSERT_EQUAL_UINT32(2u, done_log[1].tag);
    TEST_ASSERT_EQUAL_UINT(0u, ev_n); /* no recovery */
    TEST_ASSERT_EQUAL_UINT32(0u, tr.stats.aborts);
    TEST_ASSERT_EQUAL_UINT32(0u, tr.stats.unclaimed);
    TEST_ASSERT_EQUAL_UINT(1u, zac.by_func[F_SEND_DATA]);
}

/* ZW-003 (§7, ZW12): the response lost, then a callback that is not OK (NoAck): the command
 * keeps the response-timeout failure, with the callback attached; the module answered, so it
 * is not reported as not answering. */
static void test_send_data_lost_response_then_nok_callback(void)
{
    static const uint8_t no_ack[] = {0x01, 0x00, 0x03};
    script_send_data_without_response(11000, no_ack, sizeof no_ack);
    submit_send_data(1);
    run(30000);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_RESPONSE_TIMEOUT, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT64(11000u, done_log[0].at);
    TEST_ASSERT_TRUE(done_log[0].has_callback);
    TEST_ASSERT_EQUAL_HEX8(0x01u, done_log[0].callback[1]);
    TEST_ASSERT_EQUAL_UINT(0u, ev_n);
    TEST_ASSERT_EQUAL_UINT8(1u, done_log[0].transmissions);
}

/* ZW-003 (§7, ZW12): the response lost and no callback either: SendDataAbort 20 s after the ACK
 * (01 03 00 16 EA, its ACK expected), and at 30 s the command fails with the response timeout
 * and the module is not answering - the bound a SendData always had. */
static void test_send_data_lost_response_and_no_callback(void)
{
    static const uint8_t abort_frame[] = {0x01, 0x03, 0x00, 0x16, 0xEA};
    script_send_data_without_response(0, NULL, 0);
    submit_send_data(1);
    run(19999);
    TEST_ASSERT_EQUAL_UINT(1u, zac.heard);
    TEST_ASSERT_EQUAL_UINT(0u, done_n);
    run(20000);
    TEST_ASSERT_EQUAL_UINT(2u, zac.heard);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(abort_frame, heard(1)->raw, sizeof abort_frame);
    TEST_ASSERT_EQUAL_UINT64(30000u, zw_transport_next_deadline(&tr));
    run(29999);
    TEST_ASSERT_EQUAL_UINT(0u, done_n);
    TEST_ASSERT_EQUAL_UINT(0u, ev_n);
    run(30000);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_RESPONSE_TIMEOUT, done_log[0].status);
    TEST_ASSERT_FALSE(done_log[0].has_callback);
    TEST_ASSERT_EQUAL_UINT(1u, ev_n);
    TEST_ASSERT_EQUAL_INT(ZW_TRANSPORT_NOT_ANSWERING, ev_log[0].ev);
    TEST_ASSERT_EQUAL_UINT64(30000u, ev_log[0].at);
    TEST_ASSERT_EQUAL_UINT32(1u, tr.stats.aborts);
    TEST_ASSERT_EQUAL_UINT32(0u, tr.stats.stray);
}

/* ZW-003 (§7, ZW12): the response lost; the abort provokes a NoAck callback: the response
 * timeout, without a recovery (the module answered). */
static void test_send_data_lost_response_abort_provokes_callback(void)
{
    static const uint8_t no_ack[] = {0x01};
    fake_rule_t r = rule_for(F_SEND_DATA, NULL, 0, 0);
    r.callback_on_abort = true;
    set_answer(&r.abort_callback, 0, no_ack, sizeof no_ack);
    script(&r);
    submit_send_data(1);
    run(40000);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_RESPONSE_TIMEOUT, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT64(20000u, done_log[0].at);
    TEST_ASSERT_EQUAL_UINT(0u, ev_n);
}

/* ZW-003 (§7, ZW12): while the lost response's callback is awaited, a callback with an
 * unrelated (or an old command's) ID and one with the right ID but another FUNC are the
 * module's own frames, never taken; the right one then ends the command OK. */
static void test_send_data_lost_response_ignores_other_callbacks(void)
{
    static const uint8_t tx_ok[] = {0x00, 0x00, 0x03};
    static const uint8_t old_id[] = {0x99, 0x00}, other_func[] = {0x01, 0x00};
    script_send_data_without_response(12000, tx_ok, sizeof tx_ok);
    TEST_ASSERT_TRUE(fake_zac93_send_at(&zac, 10500, ZW_TYPE_REQUEST, F_SEND_DATA, old_id,
                                        sizeof old_id));
    TEST_ASSERT_TRUE(fake_zac93_send_at(&zac, 11000, ZW_TYPE_REQUEST, F_SUC_ROUTE, other_func,
                                        sizeof other_func));
    submit_send_data(1);
    run(11500);
    TEST_ASSERT_EQUAL_UINT(0u, done_n);
    TEST_ASSERT_EQUAL_UINT(2u, frame_n);
    TEST_ASSERT_EQUAL_HEX8(0x99u, frame_log[0].payload[0]);
    TEST_ASSERT_EQUAL_HEX8(F_SUC_ROUTE, frame_log[1].func);
    run(20000);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT64(12000u, done_log[0].at);
    TEST_ASSERT_EQUAL_UINT(0u, ev_n);
}

/* ZW-003 (§7, ZW12): an unexpected SerialAPIStarted while the lost response's callback is
 * awaited ends the command as any restart does. */
static void test_send_data_lost_response_then_restart(void)
{
    script_send_data_without_response(0, NULL, 0);
    submit_send_data(1);
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&zac, 15000, 0x03));
    run(30000);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_MODULE_RESTARTED, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT64(15000u, done_log[0].at);
    TEST_ASSERT_EQUAL_UINT(1u, ev_n);
    TEST_ASSERT_EQUAL_INT(ZW_TRANSPORT_MODULE_RESTARTED, ev_log[0].ev);
}

/* ---- Restarts (§2, §6.8) ------------------------------------------------------------------ */

/* ZW-003: a SerialAPIStarted nobody asked for fails the command in flight and is reported
 * with its frame (the wake-up reason: watchdog); the next command then goes as usual. */
static void test_unexpected_serial_api_started(void)
{
    script_version();
    submit_version(1); /* scripted: answered at 5 ms - but the module restarts at 3 */
    submit_version(2);
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&zac, 3, 0x03));
    run(3);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_MODULE_RESTARTED, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT64(3u, done_log[0].at);
    TEST_ASSERT_EQUAL_UINT(1u, ev_n);
    TEST_ASSERT_EQUAL_INT(ZW_TRANSPORT_MODULE_RESTARTED, ev_log[0].ev);
    TEST_ASSERT_EQUAL_HEX8(F_STARTED, ev_log[0].func);
    TEST_ASSERT_EQUAL_HEX8(0x03u, ev_log[0].reason);
    run(1000);
    TEST_ASSERT_EQUAL_UINT(2u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[1].status);
    TEST_ASSERT_EQUAL_UINT64(8u, done_log[1].at);
    TEST_ASSERT_EQUAL_UINT32(1u, tr.stats.restarts);
    /* while idle: reported, nothing to fail */
    TEST_ASSERT_TRUE(fake_zac93_restart_at(&zac, 2000, 0x09));
    run(3000);
    TEST_ASSERT_EQUAL_UINT(2u, done_n);
    TEST_ASSERT_EQUAL_UINT(2u, ev_n);
    TEST_ASSERT_EQUAL_HEX8(0x09u, ev_log[1].reason);
}

/* ZW-003: SoftReset (01 03 00 08 F4) has no response; the SerialAPIStarted that follows its
 * ACK finishes it (reason 07, software reset) and is not reported as unexpected. */
static void test_soft_reset(void)
{
    static const uint8_t frame[] = {0x01, 0x03, 0x00, 0x08, 0xF4};
    const zw_request_t q = request(F_SOFT_RESET, NULL, 0, 0, ZW_PRIO_MODULE, 1);
    submit(&q);
    run(1000);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(frame, heard(0)->raw, sizeof frame);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT64(20u, done_log[0].at);
    TEST_ASSERT_TRUE(done_log[0].has_callback);
    TEST_ASSERT_EQUAL_HEX8(0x07u, done_log[0].callback[0]);
    TEST_ASSERT_EQUAL_UINT(0u, ev_n);
    TEST_ASSERT_EQUAL_UINT32(0u, tr.stats.restarts);
}

/* ZW-003: a SoftReset's ACK damaged in its high nibble (F6) is taken as its ACK (§6.8); for any
 * other command it is not an ACK - the ACK timeout runs (an ACK-only command here, so no
 * answer stands in for the lost ACK). */
static void test_soft_reset_damaged_ack(void)
{
    const zw_request_t q = request(F_SOFT_RESET, NULL, 0, 0, ZW_PRIO_MODULE, 1);
    zac.fault.corrupt_in = 0;
    zac.fault.corrupt_xor = 0xF0;
    submit(&q);
    run(1000);
    TEST_ASSERT_EQUAL_UINT(1u, zac.heard);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT64(20u, done_log[0].at);
    TEST_ASSERT_EQUAL_UINT(1u, text_n);
    TEST_ASSERT_EQUAL_HEX8(0xF6u, text_log[0]);

    const uint8_t nif[] = {0x01, 0x02, 0x01, 0x00};
    const zw_request_t q2 = request(F_APP_NODE_INFO, nif, sizeof nif, 0, ZW_PRIO_MODULE, 2);
    zac.fault.corrupt_in = 0;
    submit(&q2);
    run(5000);
    TEST_ASSERT_EQUAL_UINT(2u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[1].status);
    TEST_ASSERT_EQUAL_UINT8(2u, done_log[1].transmissions); /* F6 was no ACK here */
    TEST_ASSERT_EQUAL_UINT64(1000u + 1700u, done_log[1].at);
}

/* ZW-003, ZW-004, serial-api.md §6.8's decision for the ZAC93: a SoftReset's ACK arriving with a
 * random high nibble - a real firmware bug of the SDK the 700 and 800 series share (Z-Wave JS's
 * corruptACKAfterSoftReset.test.ts, their issue #6399, "7.19.x, not fixed so far") - is taken as
 * its ACK, whatever the nibble, so a module slower to restart than the 1600 ms ACK timeout is not
 * sent a second SoftReset; the SerialAPIStarted then ends it. Only the bug's shape counts: a lone
 * byte. Text while the ACK is awaited - the bootloader's menu with its 'v', or a 'v' glued to
 * other bytes - is no ACK: without one the SoftReset goes again at 1700 ms as usual. */
static void test_soft_reset_corrupt_ack_any_high_nibble(void)
{
    const zw_request_t q = request(F_SOFT_RESET, NULL, 0, 0, ZW_PRIO_MODULE, 1);
    for (unsigned hi = 1u; hi <= 0x0Fu; hi++) {
        tearDown();
        setUp();
        zac.soft_reset_ms = 2500u; /* longer than the ACK timeout */
        zac.fault.corrupt_in = 0;  /* its ACK, the first byte it sends */
        zac.fault.corrupt_xor = (uint8_t)(hi << 4);
        submit(&q);
        run(6000);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, zac.heard, "the SoftReset went again");
        TEST_ASSERT_EQUAL_UINT(1u, done_n);
        TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
        TEST_ASSERT_EQUAL_UINT64(2500u, done_log[0].at);
        TEST_ASSERT_EQUAL_UINT(1u, text_n);
        TEST_ASSERT_EQUAL_HEX8((uint8_t)(hi << 4 | 0x06u), text_log[0]);
        TEST_ASSERT_EQUAL_UINT(0u, ev_n);
    }
    static const uint8_t MENU_LINE[] = "\r\nGecko Bootloader v2.4.2\r\n";
    static const uint8_t V_AND_MORE[] = {0x76, 0x0D, 0x0A};
    const struct {
        const uint8_t *text;
        size_t len;
    } TEXTS[] = {{MENU_LINE, sizeof MENU_LINE - 1u}, {V_AND_MORE, sizeof V_AND_MORE}};
    for (size_t i = 0; i < sizeof TEXTS / sizeof TEXTS[0]; i++) {
        tearDown();
        setUp();
        zac.soft_reset_ms = 2500u;
        zac.fault.drop_next_ack = true; /* it restarts, its ACK lost; text comes instead */
        TEST_ASSERT_TRUE(fake_zac93_send_raw_at(&zac, 5u, TEXTS[i].text, TEXTS[i].len));
        submit(&q);
        run(6000);
        TEST_ASSERT_EQUAL_UINT(TEXTS[i].len, text_n);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(2u, zac.heard, "text was taken for the ACK");
        TEST_ASSERT_EQUAL_UINT64(1700u, heard(1)->at);
        TEST_ASSERT_EQUAL_UINT(1u, done_n);
        TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status); /* its SerialAPIStarted all the same */
    }
}

/* ZW-003: a SoftReset whose SerialAPIStarted never comes fails 5 s after its ACK. */
static void test_soft_reset_without_restart(void)
{
    const zw_request_t q = request(F_SOFT_RESET, NULL, 0, 0, ZW_PRIO_MODULE, 1);
    zac.fault.ignore_soft_reset = true;
    submit(&q);
    run(4999);
    TEST_ASSERT_EQUAL_UINT(0u, done_n);
    run(5000);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_NO_RESTART, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT(0u, ev_n);
}

/* ---- The scheduler ------------------------------------------------------------------------ */

static void submit_tagged(zw_prio_t prio, uint8_t tag)
{
    const uint8_t payload[] = {tag};
    const zw_request_t q = request(F_APP_NODE_INFO, payload, sizeof payload, 0, prio, tag);
    submit(&q);
}

/* ZW-003: priorities - replies, module commands, node commands, interview, background; first
 * come first served within a class. A nonce reply queued behind node commands goes next. */
static void test_priorities(void)
{
    script_version();
    submit_version(100); /* in flight first */
    run(0);
    submit_tagged(ZW_PRIO_NODE, 1);
    submit_tagged(ZW_PRIO_NODE, 2);
    submit_tagged(ZW_PRIO_BACKGROUND, 3);
    submit_tagged(ZW_PRIO_INTERVIEW, 4);
    submit_tagged(ZW_PRIO_REPLY, 5);
    submit_tagged(ZW_PRIO_MODULE, 6);
    submit_tagged(ZW_PRIO_REPLY, 7);
    TEST_ASSERT_EQUAL_UINT(7u, zw_transport_queued(&tr));
    run(1000);
    static const uint8_t order[] = {5, 7, 6, 1, 2, 4, 3};
    TEST_ASSERT_EQUAL_UINT(8u, zac.heard);
    for (size_t i = 0; i < sizeof order; i++) {
        TEST_ASSERT_EQUAL_UINT8(order[i], heard(i + 1)->payload[0]);
    }
    TEST_ASSERT_EQUAL_UINT(8u, done_n);
}

/* ZW-003: the queue has a fixed size: one more is refused with FULL; malformed requests with
 * INVALID; both counted. */
static void test_queue_full_and_invalid(void)
{
    for (unsigned i = 0; i < ZW_TRANSPORT_QUEUE_LEN; i++) {
        submit_tagged(ZW_PRIO_NODE, (uint8_t)i);
    }
    const uint8_t payload[] = {0x00};
    zw_request_t q = request(F_APP_NODE_INFO, payload, sizeof payload, 0, ZW_PRIO_REPLY, 99);
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_FULL, zw_transport_submit(&tr, &q));
    TEST_ASSERT_EQUAL_UINT32(1u, tr.stats.refused);
    TEST_ASSERT_EQUAL_UINT(1u, zw_transport_cancel(&tr, 0));

    static uint8_t big[ZW_PAYLOAD_MAX + 1];
    const uint8_t cb[] = {0x05, 0x00};
    q = request(F_APP_NODE_INFO, big, sizeof big, 0, ZW_PRIO_NODE, 1); /* too long */
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_INVALID, zw_transport_submit(&tr, &q));
    q = request(F_APP_NODE_INFO, NULL, 2, 0, ZW_PRIO_NODE, 1); /* no payload */
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_INVALID, zw_transport_submit(&tr, &q));
    q = request(F_APP_NODE_INFO, cb, sizeof cb, 0x04, ZW_PRIO_NODE, 1); /* unknown expect bit */
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_INVALID, zw_transport_submit(&tr, &q));
    q = request(F_APP_NODE_INFO, cb, sizeof cb, 0, ZW_PRIO_COUNT, 1); /* no such class */
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_INVALID, zw_transport_submit(&tr, &q));
    q = request(F_NEIGHBOURS, cb, sizeof cb, ZW_EXPECT_CALLBACK, ZW_PRIO_NODE, 1);
    q.callback_id_at = 2; /* past the payload */
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_INVALID, zw_transport_submit(&tr, &q));
    q = request(F_NEIGHBOURS, cb, sizeof cb, ZW_EXPECT_CALLBACK, ZW_PRIO_NODE, 1);
    q.callback_timeout_ms = 0; /* a callback with no timeout */
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_INVALID, zw_transport_submit(&tr, &q));
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_INVALID, zw_transport_submit(&tr, NULL));
    TEST_ASSERT_EQUAL_UINT32(8u, tr.stats.refused);
    /* SendData's callback timing is its own: no timeout needed */
    q = request(F_SEND_DATA, SEND_BASIC_SET, sizeof SEND_BASIC_SET,
                ZW_EXPECT_RESPONSE | ZW_EXPECT_CALLBACK, ZW_PRIO_NODE, 1);
    q.callback_timeout_ms = 0;
    TEST_ASSERT_EQUAL_INT(ZW_SUBMIT_OK, zw_transport_submit(&tr, &q));
}

/* ZW-003: the dispatch gate. Closed to MODULE (start-up, recovery) only module commands start -
 * even a nonce reply waits, it is SendData; held requests keep their place and nothing is due;
 * each level opened lets the next class out: replies, then node commands, then the rest. */
static void test_gate_holds_all_but_module_commands(void)
{
    zw_transport_set_gate(&tr, ZW_PRIO_MODULE);
    submit_tagged(ZW_PRIO_NODE, 1);
    submit_tagged(ZW_PRIO_REPLY, 2);
    submit_tagged(ZW_PRIO_BACKGROUND, 3);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, zw_transport_next_deadline(&tr));
    run(1000);
    TEST_ASSERT_EQUAL_UINT(0u, zac.heard);
    submit_tagged(ZW_PRIO_MODULE, 4);
    TEST_ASSERT_EQUAL_UINT64(0u, zw_transport_next_deadline(&tr));
    run(2000);
    TEST_ASSERT_EQUAL_UINT(1u, zac.heard);
    TEST_ASSERT_EQUAL_UINT8(4u, heard(0)->payload[0]);
    TEST_ASSERT_EQUAL_UINT(3u, zw_transport_queued(&tr));
    zw_transport_set_gate(&tr, ZW_PRIO_REPLY);
    run(3000);
    TEST_ASSERT_EQUAL_UINT(2u, zac.heard);
    TEST_ASSERT_EQUAL_UINT8(2u, heard(1)->payload[0]);
    zw_transport_set_gate(&tr, ZW_PRIO_NODE);
    run(4000);
    TEST_ASSERT_EQUAL_UINT(3u, zac.heard);
    TEST_ASSERT_EQUAL_UINT8(1u, heard(2)->payload[0]);
    zw_transport_set_gate(&tr, ZW_PRIO_BACKGROUND);
    run(5000);
    TEST_ASSERT_EQUAL_UINT(4u, zac.heard);
    TEST_ASSERT_EQUAL_UINT8(3u, heard(3)->payload[0]);
    TEST_ASSERT_EQUAL_UINT(4u, done_n);
}

static void resubmit_done(void *ctx, uint32_t tag, const zw_tx_result_t *r)
{
    (void)ctx;
    record_done(tag, r);
    submit_tagged(ZW_PRIO_NODE, 9); /* allowed from done(); must not be ended with the rest */
}

/* ZW-003: link down - every queued request but the module commands ends with LINK_DOWN, never
 * sent, in the order they would have gone; a done() that queues again keeps its new request; the
 * command in flight goes on. */
static void test_fail_queued_on_link_down(void)
{
    const uint8_t reply_payload[] = {2};
    script_version();
    submit_version(100);
    run(0); /* in flight: its response comes at 5 ms */
    zw_transport_set_gate(&tr, ZW_PRIO_MODULE);
    submit_tagged(ZW_PRIO_NODE, 1);
    zw_request_t q = request(F_APP_NODE_INFO, reply_payload, sizeof reply_payload, 0,
                             ZW_PRIO_REPLY, 2);
    q.done = resubmit_done;
    submit(&q);
    submit_tagged(ZW_PRIO_BACKGROUND, 3);
    submit_tagged(ZW_PRIO_MODULE, 4);
    const unsigned below_module = ZW_PRIO_ALL & ~ZW_PRIO_BIT(ZW_PRIO_MODULE);
    TEST_ASSERT_EQUAL_UINT(3u, zw_transport_fail_queued(&tr, below_module, ZW_TX_LINK_DOWN));
    TEST_ASSERT_EQUAL_UINT(3u, done_n);
    static const uint32_t order[] = {2, 1, 3};
    for (size_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_UINT32(order[i], done_log[i].tag);
        TEST_ASSERT_EQUAL_INT(ZW_TX_LINK_DOWN, done_log[i].status);
        TEST_ASSERT_EQUAL_UINT8(F_APP_NODE_INFO, done_log[i].func);
        TEST_ASSERT_EQUAL_UINT8(0u, done_log[i].transmissions);
        TEST_ASSERT_FALSE(done_log[i].has_response || done_log[i].has_callback);
    }
    TEST_ASSERT_EQUAL_UINT(2u, zw_transport_queued(&tr)); /* the module command and the new one */
    TEST_ASSERT_TRUE(zw_transport_busy(&tr));
    run(1000);
    TEST_ASSERT_EQUAL_UINT(5u, done_n);
    TEST_ASSERT_EQUAL_UINT32(100u, done_log[3].tag); /* the one in flight, as usual */
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[3].status);
    TEST_ASSERT_EQUAL_UINT32(4u, done_log[4].tag);
    TEST_ASSERT_EQUAL_UINT(2u, zac.heard);
    TEST_ASSERT_EQUAL_UINT(1u, zw_transport_queued(&tr)); /* 9, held by the gate */
    TEST_ASSERT_EQUAL_UINT32(3u, tr.stats.dropped);
}

/* ZW-003: cancel by tag drops the queued requests with that tag, without their done(); the
 * command in flight with the same tag goes on. */
static void test_cancel_by_tag(void)
{
    script_version();
    submit_version(1);
    run(0);
    submit_tagged(ZW_PRIO_NODE, 1);
    submit_tagged(ZW_PRIO_NODE, 2);
    submit_tagged(ZW_PRIO_NODE, 1);
    submit_tagged(ZW_PRIO_NODE, 3);
    TEST_ASSERT_EQUAL_UINT(2u, zw_transport_cancel(&tr, 1));
    TEST_ASSERT_EQUAL_UINT(0u, zw_transport_cancel(&tr, 1));
    run(1000);
    TEST_ASSERT_EQUAL_UINT(3u, zac.heard);
    TEST_ASSERT_EQUAL_UINT8(2u, heard(1)->payload[0]);
    TEST_ASSERT_EQUAL_UINT8(3u, heard(2)->payload[0]);
    TEST_ASSERT_EQUAL_UINT(3u, done_n);
    TEST_ASSERT_EQUAL_UINT32(1u, done_log[0].tag); /* the one in flight */
    TEST_ASSERT_EQUAL_UINT32(2u, tr.stats.cancelled);
}

/* ---- The wire ----------------------------------------------------------------------------- */

/* ZW-003: a corrupted byte on the wire: zw_link NAKs the damaged response, the module sends it
 * again, the command gets it intact. */
static void test_corrupted_byte_nak_and_resend(void)
{
    script_version();
    zac.fault.corrupt_in = 5; /* past the ACK and SOF LEN TYPE FUNC: the first payload byte */
    submit_version(1);
    run(1000);
    TEST_ASSERT_EQUAL_UINT32(1u, wire.naks);
    TEST_ASSERT_EQUAL_UINT32(1u, zac.resent);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(VERSION, done_log[0].response, sizeof VERSION);
    TEST_ASSERT_EQUAL_UINT8(1u, done_log[0].transmissions);
}

/* ZW-003: bytes the parser discards while idle reach the owner as text, in order - the
 * bootloader's banner that start-up step 1 looks for (§5, phase B) - while a command waits,
 * without disturbing it. */
static void test_discarded_bytes_reach_the_owner_as_text(void)
{
    static const char banner[] = "Gecko Bootloader v2.4.2\r\nBL > ";
    script_version();
    TEST_ASSERT_TRUE(fake_zac93_send_raw_at(&zac, 2, (const uint8_t *)banner, sizeof banner - 1));
    submit_version(1);
    run(1000);
    TEST_ASSERT_EQUAL_UINT(sizeof banner - 1, text_n);
    TEST_ASSERT_EQUAL_MEMORY(banner, text_log, sizeof banner - 1);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
}

/* ZW-003: ACK, NAK or CAN when nothing waits for one: counted, otherwise ignored. */
static void test_stray_control_bytes(void)
{
    static const uint8_t bytes[] = {0x06, 0x15, 0x18};
    TEST_ASSERT_TRUE(fake_zac93_send_raw_at(&zac, 10, bytes, sizeof bytes));
    run(1000);
    TEST_ASSERT_EQUAL_UINT32(3u, tr.stats.stray);
    TEST_ASSERT_EQUAL_UINT(0u, done_n);
    TEST_ASSERT_EQUAL_UINT(0u, ev_n);
}

/* ZW-003: a write the link could not take counts as a transmission without ACK: again 100 ms
 * later. */
static void test_write_failure_is_a_lost_frame(void)
{
    script_version();
    fail_writes = 1;
    submit_version(1);
    run(1000);
    TEST_ASSERT_EQUAL_UINT(1u, zac.heard);
    TEST_ASSERT_EQUAL_UINT64(100u, heard(0)->at);
    TEST_ASSERT_EQUAL_UINT(1u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
    TEST_ASSERT_EQUAL_UINT8(2u, done_log[0].transmissions);
    TEST_ASSERT_EQUAL_UINT32(1u, tr.stats.write_failures);
}

/* ---- Owner callbacks ---------------------------------------------------------------------- */

static void reentrant_done(void *ctx, uint32_t tag, const zw_tx_result_t *r)
{
    (void)ctx;
    record_done(tag, r);
    if (tag == 1) {
        zw_transport_poll(&tr, wire.now);                /* refused */
        zw_transport_on_control(&tr, wire.now, ZW_ACK);  /* refused */
        submit_tagged(ZW_PRIO_NODE, 2);                  /* allowed */
        TEST_ASSERT_EQUAL_UINT(0u, zw_transport_cancel(&tr, 42)); /* allowed */
    }
}

/* ZW-003: a completion callback may queue the next request, which goes next; feeding the link
 * or polling from inside it is refused and counted, and changes nothing. */
static void test_done_may_submit_but_not_poll(void)
{
    const uint8_t payload[] = {0x01};
    zw_request_t q = request(F_APP_NODE_INFO, payload, sizeof payload, 0, ZW_PRIO_NODE, 1);
    q.done = reentrant_done;
    submit(&q);
    run(1000);
    TEST_ASSERT_EQUAL_UINT32(2u, tr.stats.reentries);
    TEST_ASSERT_EQUAL_UINT(2u, zac.heard);
    TEST_ASSERT_EQUAL_UINT8(2u, heard(1)->payload[0]);
    TEST_ASSERT_EQUAL_UINT(2u, done_n);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[0].status);
    TEST_ASSERT_EQUAL_INT(ZW_TX_OK, done_log[1].status);
    TEST_ASSERT_EQUAL_UINT32(0u, tr.stats.stray);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_command_with_response);
    RUN_TEST(test_command_with_callback);
    RUN_TEST(test_command_with_response_and_callback);
    RUN_TEST(test_callback_ids_wrap_and_skip_zero);
    RUN_TEST(test_nak_retries_then_not_answering);
    RUN_TEST(test_nak_twice_then_ok);
    RUN_TEST(test_ack_timeout_at_1600);
    RUN_TEST(test_response_before_ack_counts_as_ack);
    RUN_TEST(test_send_data_lost_ack_is_sent_once);
    RUN_TEST(test_callback_before_ack_counts_as_ack);
    RUN_TEST(test_callback_before_ack_of_command_with_response_not_taken);
    RUN_TEST(test_late_ack_is_stray);
    RUN_TEST(test_can_waits_100_ms_and_module_frame_first);
    RUN_TEST(test_three_cans_fail);
    RUN_TEST(test_response_timeout);
    RUN_TEST(test_callback_timeout);
    RUN_TEST(test_interleaved_module_frames);
    RUN_TEST(test_intermediate_then_final_callbacks);
    RUN_TEST(test_intermediate_callback_then_timeout);
    RUN_TEST(test_add_node_ends_at_ready);
    RUN_TEST(test_send_data_not_queued_three_times);
    RUN_TEST(test_send_data_not_queued_twice_then_queued);
    RUN_TEST(test_send_data_abort_at_20s_fail_at_30s);
    RUN_TEST(test_send_data_abort_provokes_callback);
    RUN_TEST(test_abort_ack_holds_the_next_command);
    RUN_TEST(test_send_data_lost_response_then_ok_callback);
    RUN_TEST(test_send_data_lost_response_then_nok_callback);
    RUN_TEST(test_send_data_lost_response_and_no_callback);
    RUN_TEST(test_send_data_lost_response_abort_provokes_callback);
    RUN_TEST(test_send_data_lost_response_ignores_other_callbacks);
    RUN_TEST(test_send_data_lost_response_then_restart);
    RUN_TEST(test_unexpected_serial_api_started);
    RUN_TEST(test_soft_reset);
    RUN_TEST(test_soft_reset_damaged_ack);
    RUN_TEST(test_soft_reset_corrupt_ack_any_high_nibble);
    RUN_TEST(test_soft_reset_without_restart);
    RUN_TEST(test_priorities);
    RUN_TEST(test_queue_full_and_invalid);
    RUN_TEST(test_gate_holds_all_but_module_commands);
    RUN_TEST(test_fail_queued_on_link_down);
    RUN_TEST(test_cancel_by_tag);
    RUN_TEST(test_corrupted_byte_nak_and_resend);
    RUN_TEST(test_discarded_bytes_reach_the_owner_as_text);
    RUN_TEST(test_stray_control_bytes);
    RUN_TEST(test_write_failure_is_a_lost_frame);
    RUN_TEST(test_done_may_submit_but_not_poll);
    return UNITY_END();
}
