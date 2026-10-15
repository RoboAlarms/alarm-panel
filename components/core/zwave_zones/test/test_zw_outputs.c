/* ZW-034, ZW-035 (docs/features/36): what the panel's Z-Wave devices switch, against a scripted
 * radio - outputs found from the node cache (a multi-relay through its end points only), a Set
 * done only when the device confirms it, the same state from a press at the device, stale and
 * overlapping commands refused, and a garage door made of a relay and a tilt sensor: never a
 * blind press, a press released and confirmed, the door watched to its goal, a second press held
 * back during travel, a relay that won't let go reported. */
#include <string.h>

#include "unity.h"
#include "zwave_proto/zw_cc_switch.h"
#include "zwave_zones/zw_outputs.h"

static zwo_t O;
static struct {
    uint16_t node;
    uint8_t ep;
    uint8_t cmd[8];
    size_t len;
    uint32_t tag;
} SENT[32];
static size_t N;
static bool SEND_FAILS;
static zwo_position_t POS;
static uint32_t RES_CMD;
static zwo_result_t RES;
static unsigned RESULTS, CHANGES, REFUSED;
static uint64_t NOW;

static bool p_send(void *ctx, uint16_t node, uint8_t ep, const uint8_t *cmd, size_t len,
                   uint32_t tag)
{
    (void)ctx;
    if (SEND_FAILS) {
        REFUSED++;
        return false;
    }
    TEST_ASSERT_TRUE(N < 32u && len <= 8u);
    SENT[N].node = node;
    SENT[N].ep = ep;
    memcpy(SENT[N].cmd, cmd, len);
    SENT[N].len = len;
    SENT[N].tag = tag;
    N++;
    return true;
}
static zwo_position_t p_position(void *ctx, uint16_t zone)
{
    (void)ctx;
    TEST_ASSERT_EQUAL_UINT16(12u, zone);
    return POS;
}
static void p_result(void *ctx, uint32_t cmd, zwo_result_t r)
{
    (void)ctx;
    RES_CMD = cmd;
    RES = r;
    RESULTS++;
}
static void p_changed(void *ctx)
{
    (void)ctx;
    CHANGES++;
}

static zw_node_record_t REC;

static void record(uint16_t node, uint16_t gen, const uint8_t *root, size_t n, bool relays)
{
    memset(&REC, 0, sizeof REC);
    REC.node_id = node;
    REC.generation = gen;
    for (size_t i = 0; i < n; i++) {
        REC.ccs[i].cc = root[i];
        REC.ccs[i].supported = true;
    }
    REC.cc_count = (uint8_t)n;
    if (relays) {
        for (uint8_t ep = 1u; ep < ZW_NODE_ENDPOINTS_MAX; ep++) {
            REC.endpoints[ep].used = true;
            REC.endpoints[ep].cc_count = 1u;
            REC.endpoints[ep].ccs[0] = 0x25u;
        }
    }
}

void setUp(void)
{
    const zwo_port_t port = {NULL, p_send, p_position, p_result, p_changed};
    zwo_init(&O, &port);
    N = 0u;
    SEND_FAILS = false;
    POS = ZWO_POS_CLOSED;
    RESULTS = CHANGES = REFUSED = 0u;
    RES_CMD = 0u;
    NOW = 100000u;
}
void tearDown(void) {}

static const uint8_t *last(void) { return SENT[N - 1u].cmd; }
static void done(bool ok) { (void)zwo_send_done(&O, NOW, SENT[N - 1u].tag, ok); }
static void level_report(uint16_t node, uint8_t ep, uint8_t cc, uint8_t value)
{
    const uint8_t r[3] = {cc, ZW_CMD_SWITCH_REPORT, value};
    TEST_ASSERT_TRUE(zwo_received(&O, NOW, node, ep, r, sizeof r));
}
static void run(uint64_t ms)
{
    for (uint64_t t = 0; t < ms; t += 100u) {
        NOW += 100u;
        zwo_poll(&O, NOW);
    }
}

/* ZW-034: a switch, a dimmer and a multi-relay (its end points only, not its mirroring root). */
static void test_scan(void)
{
    static const uint8_t SW[] = {0x5E, 0x25, 0x86};
    static const uint8_t DIM[] = {0x5E, 0x26, 0x25};
    static const uint8_t MULTI[] = {0x5E, 0x25, 0x60};
    record(4u, 1u, SW, sizeof SW, false);
    TEST_ASSERT_EQUAL_UINT(1u, zwo_scan(&O, &REC));
    TEST_ASSERT_EQUAL_UINT8(ZWO_SWITCH, O.out[zwo_find(&O, 4u, 0u)].kind);
    TEST_ASSERT_EQUAL_INT8(-1, O.out[zwo_find(&O, 4u, 0u)].level);
    record(5u, 1u, DIM, sizeof DIM, false);
    TEST_ASSERT_EQUAL_UINT(1u, zwo_scan(&O, &REC));
    TEST_ASSERT_EQUAL_UINT8(ZWO_DIMMER, O.out[zwo_find(&O, 5u, 0u)].kind);
    record(6u, 1u, MULTI, sizeof MULTI, true);
    TEST_ASSERT_EQUAL_UINT(3u, zwo_scan(&O, &REC));
    TEST_ASSERT_EQUAL_INT(-1, zwo_find(&O, 6u, 0u));
    TEST_ASSERT_EQUAL_UINT8(ZWO_RELAY, O.out[zwo_find(&O, 6u, 3u)].kind);

    /* scanned again: kept with its level; under a new generation (the node ID given to another
     * device): its outputs start again */
    level_report(4u, 0u, 0x25u, 0xFFu);
    record(4u, 1u, SW, sizeof SW, false);
    (void)zwo_scan(&O, &REC);
    TEST_ASSERT_EQUAL_INT8(100, O.out[zwo_find(&O, 4u, 0u)].level);
    record(4u, 2u, SW, sizeof SW, false);
    (void)zwo_scan(&O, &REC);
    TEST_ASSERT_EQUAL_INT8(-1, O.out[zwo_find(&O, 4u, 0u)].level);
    zwo_forget(&O, 6u);
    TEST_ASSERT_EQUAL_INT(-1, zwo_find(&O, 6u, 1u));
    /* a root Report of a multi-relay isn't an output's, but no one else's either */
    record(6u, 1u, MULTI, sizeof MULTI, true);
    (void)zwo_scan(&O, &REC);
    level_report(6u, 0u, 0x25u, 0xFFu);
    TEST_ASSERT_EQUAL_INT8(-1, O.out[zwo_find(&O, 6u, 1u)].level);
}

/* ZW-034: a Set is done only when the device says so; a press at the device lands in the same
 * state; commands too old, overlapping or out of range are refused; one never confirmed ends
 * TIMEOUT with the level the device last said. */
static void test_set(void)
{
    static const uint8_t SW[] = {0x25};
    static const uint8_t DIM[] = {0x26};
    record(4u, 1u, SW, sizeof SW, false);
    (void)zwo_scan(&O, &REC);
    record(5u, 1u, DIM, sizeof DIM, false);
    (void)zwo_scan(&O, &REC);
    const size_t sw = (size_t)zwo_find(&O, 4u, 0u);
    const size_t dim = (size_t)zwo_find(&O, 5u, 0u);

    TEST_ASSERT_EQUAL_INT(ZWO_ERR_STALE, zwo_set(&O, NOW, sw, 100, NOW - ZWO_STALE_MS - 1u, 7u));
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_STALE, zwo_set(&O, NOW, sw, 100, NOW + 1u, 7u));
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_ARG, zwo_set(&O, NOW, sw, 50, NOW, 7u));
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_UNKNOWN, zwo_set(&O, NOW, ZWO_OUTPUTS, 0, NOW, 7u));
    TEST_ASSERT_EQUAL_UINT(0u, N);

    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_set(&O, NOW, sw, 100, NOW, 7u));
    const uint8_t on[] = {0x25, 0x01, 0xFF};
    TEST_ASSERT_EQUAL_MEMORY(on, last(), 3u);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_BUSY, zwo_set(&O, NOW, sw, 0, NOW, 8u));
    done(true);
    run(ZWO_VERIFY_MS);
    TEST_ASSERT_EQUAL_HEX8(ZW_CMD_SWITCH_GET, last()[1]); /* it didn't report: asked */
    TEST_ASSERT_EQUAL_UINT(0u, RESULTS);
    level_report(4u, 0u, 0x25u, 0xFFu);
    TEST_ASSERT_EQUAL_UINT(1u, RESULTS);
    TEST_ASSERT_EQUAL_UINT32(7u, RES_CMD);
    TEST_ASSERT_EQUAL_INT(ZWO_OK, RES);

    /* the dimmer's Report before the Set's own end */
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_set(&O, NOW, dim, 40, NOW, 9u));
    const uint8_t forty[] = {0x26, 0x01, 40};
    TEST_ASSERT_EQUAL_MEMORY(forty, last(), 3u);
    level_report(5u, 0u, 0x26u, 40u);
    TEST_ASSERT_EQUAL_INT(ZWO_OK, RES);
    TEST_ASSERT_EQUAL_UINT32(9u, RES_CMD);
    done(true); /* late: nothing more */
    TEST_ASSERT_EQUAL_UINT(2u, RESULTS);
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_set(&O, NOW, dim, 100, NOW, 10u));
    TEST_ASSERT_EQUAL_HEX8(0x63u, last()[2]); /* 99 % is the top */
    level_report(5u, 0u, 0x26u, 0x63u);
    TEST_ASSERT_EQUAL_INT(ZWO_OK, RES);
    TEST_ASSERT_EQUAL_INT8(100, O.out[dim].level);

    /* someone at the switch: the same state */
    const unsigned before = CHANGES;
    level_report(4u, 0u, 0x25u, 0x00u);
    TEST_ASSERT_EQUAL_INT8(0, O.out[sw].level);
    TEST_ASSERT_TRUE(CHANGES > before);

    /* never confirmed: TIMEOUT, the level as the device last said */
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_set(&O, NOW, sw, 100, NOW, 11u));
    done(true);
    run(ZWO_SET_MS);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_TIMEOUT, RES);
    TEST_ASSERT_EQUAL_INT8(0, O.out[sw].level);
    /* the radio can't send: said at once */
    SEND_FAILS = true;
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_SEND, zwo_set(&O, NOW, sw, 100, NOW, 12u));
    SEND_FAILS = false;
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_set(&O, NOW, sw, 100, NOW, 13u));
    done(false);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_SEND, RES);
    TEST_ASSERT_EQUAL_UINT32(13u, RES_CMD);
}

static size_t garage_setup(void)
{
    static const uint8_t MULTI[] = {0x5E, 0x25, 0x60};
    record(6u, 1u, MULTI, sizeof MULTI, true);
    (void)zwo_scan(&O, &REC);
    const size_t relay = (size_t)zwo_find(&O, 6u, 1u);
    level_report(6u, 1u, 0x25u, 0x00u);
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_assign(&O, 0u, relay, 12u, 20u));
    return relay;
}

/* The press: on, held, off confirmed, then the door. */
static void press_and_release(void)
{
    const uint8_t on[] = {0x25, 0x01, 0xFF};
    TEST_ASSERT_EQUAL_MEMORY(on, last(), 3u);
    TEST_ASSERT_EQUAL_UINT8(1u, SENT[N - 1u].ep);
    done(true);
    level_report(6u, 1u, 0x25u, 0xFFu);
    const size_t before = N;
    run(ZWO_PULSE_MS);
    TEST_ASSERT_EQUAL_UINT(before + 1u, N);
    const uint8_t off[] = {0x25, 0x01, 0x00};
    TEST_ASSERT_EQUAL_MEMORY(off, last(), 3u);
    done(true);
    level_report(6u, 1u, 0x25u, 0x00u);
    run(100u);
}

/* ZW-035: never a blind press; nothing pressed when it is already there; open and close watched
 * to the goal; the relay never switched as a plain output; a second press waits out the travel. */
static void test_garage_moves(void)
{
    const size_t relay = garage_setup();
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_ARG, zwo_set(&O, NOW, relay, 100, NOW, 1u));
    TEST_ASSERT_EQUAL_STRING("closed", zwo_garage_state(&O, 0u));
    const size_t sent = N;
    TEST_ASSERT_EQUAL_INT(ZWO_ALREADY, zwo_garage_move(&O, NOW, 0u, ZWO_POS_CLOSED, NOW, 2u));
    POS = ZWO_POS_UNKNOWN;
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_POSITION, zwo_garage_move(&O, NOW, 0u, ZWO_POS_OPEN, NOW, 2u));
    TEST_ASSERT_EQUAL_STRING("unknown", zwo_garage_state(&O, 0u));
    TEST_ASSERT_EQUAL_UINT(sent, N);
    POS = ZWO_POS_CLOSED;
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_STALE,
                          zwo_garage_move(&O, NOW, 0u, ZWO_POS_OPEN, NOW - 60000u, 2u));

    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_move(&O, NOW, 0u, ZWO_POS_OPEN, NOW, 3u));
    /* the sensor's word only: still closed, a press on its way to open */
    TEST_ASSERT_EQUAL_STRING("closed", zwo_garage_state(&O, 0u));
    zwo_position_t goal = ZWO_POS_UNKNOWN;
    TEST_ASSERT_TRUE(zwo_garage_moving(&O, 0u, &goal));
    TEST_ASSERT_EQUAL_INT(ZWO_POS_OPEN, goal);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_BUSY, zwo_garage_move(&O, NOW, 0u, ZWO_POS_CLOSED, NOW, 4u));
    press_and_release();
    run(5000u);
    TEST_ASSERT_EQUAL_UINT(0u, RESULTS);
    POS = ZWO_POS_OPEN; /* the tilt sensor: open */
    run(1000u);
    TEST_ASSERT_EQUAL_UINT(1u, RESULTS);
    TEST_ASSERT_EQUAL_INT(ZWO_OK, RES);
    TEST_ASSERT_EQUAL_UINT32(3u, RES_CMD);
    TEST_ASSERT_EQUAL_STRING("open", zwo_garage_state(&O, 0u));
    TEST_ASSERT_FALSE(zwo_garage_moving(&O, 0u, NULL));
    /* it got there: closing may go at once (B015 UX-044) */
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_move(&O, NOW, 0u, ZWO_POS_CLOSED, NOW, 6u));
    press_and_release();
    /* while it is on its way, a second press is held back (an opener reverses on one) */
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_BUSY, zwo_garage_move(&O, NOW, 0u, ZWO_POS_OPEN, NOW, 7u));
    run(20000u); /* it never got there (an obstruction, the opener reversed) */
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_NO_MOVE, RES);
    TEST_ASSERT_EQUAL_UINT32(6u, RES_CMD);
    TEST_ASSERT_EQUAL_STRING("open", zwo_garage_state(&O, 0u));
    /* "didn't get there" is said only once its whole travel time has passed: a new press may go */
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_move(&O, NOW, 0u, ZWO_POS_CLOSED, NOW, 8u));
}

/* ZW-035: a relay that won't let go is reported, never left silently on (at most ZWO_OFF_TRIES
 * offs); one already on is never pressed; a press whose on wasn't acknowledged is still released
 * before it ends (PL01). */
static void test_garage_relay_faults(void)
{
    (void)garage_setup();
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_move(&O, NOW, 0u, ZWO_POS_OPEN, NOW, 3u));
    done(true);
    run(ZWO_PULSE_MS);
    for (unsigned i = 0; i < ZWO_OFF_TRIES; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x00u, last()[2]); /* off, again */
        done(false);
    }
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_RELAY_STUCK, RES);
    TEST_ASSERT_EQUAL_UINT32(3u, RES_CMD);

    /* its off delivered but the relay keeps saying on: asked, then off again, then stuck */
    run(25000u);
    level_report(6u, 1u, 0x25u, 0x00u);
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_move(&O, NOW, 0u, ZWO_POS_OPEN, NOW, 4u));
    done(true);
    level_report(6u, 1u, 0x25u, 0xFFu);
    run(ZWO_PULSE_MS);
    done(true);
    run(60000u);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_RELAY_STUCK, RES);
    TEST_ASSERT_EQUAL_UINT32(4u, RES_CMD);
    /* still on: no new press */
    run(25000u);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_RELAY_STUCK,
                          zwo_garage_move(&O, NOW, 0u, ZWO_POS_OPEN, NOW, 5u));
    level_report(6u, 1u, 0x25u, 0x00u);
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_move(&O, NOW, 0u, ZWO_POS_OPEN, NOW, 6u));
    const unsigned results = RESULTS;
    done(false); /* not acknowledged: it may have switched all the same - released first */
    TEST_ASSERT_EQUAL_UINT(results, RESULTS);
    TEST_ASSERT_EQUAL_HEX8(0x00u, last()[2]);
    done(true);
    level_report(6u, 1u, 0x25u, 0x00u);
    run(100u);
    TEST_ASSERT_EQUAL_UINT(results + 1u, RESULTS);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_SEND, RES);
    TEST_ASSERT_EQUAL_UINT32(6u, RES_CMD);
}

/* An off sent and confirmed: the relay says off. */
static void release_confirmed(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x00u, last()[2]);
    TEST_ASSERT_EQUAL_UINT8(1u, SENT[N - 1u].ep);
    done(true);
    level_report(6u, 1u, 0x25u, 0x00u);
    run(100u);
}

/* PL01 (ZW-035): once its on is queued, a press may have switched the relay whatever the radio
 * says next - a lost acknowledgement, no answer at all, a link that drops, a relay that won't let
 * go: each ends with the relay released (at most ZWO_OFF_TRIES offs) and exactly one result. */
static void test_garage_press_unconfirmed(void)
{
    (void)garage_setup();
    /* the on delivered, its acknowledgement lost: released at once, then failed (it may have
     * moved: a new press waits out the travel) */
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_move(&O, NOW, 0u, ZWO_POS_OPEN, NOW, 3u));
    level_report(6u, 1u, 0x25u, 0xFFu); /* it did switch: its own report */
    size_t sent = N;
    done(false);
    TEST_ASSERT_EQUAL_UINT(sent + 1u, N);
    TEST_ASSERT_EQUAL_UINT(0u, RESULTS);
    TEST_ASSERT_TRUE(zwo_garage_moving(&O, 0u, NULL));
    release_confirmed();
    TEST_ASSERT_EQUAL_UINT(1u, RESULTS);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_SEND, RES);
    TEST_ASSERT_EQUAL_UINT32(3u, RES_CMD);
    TEST_ASSERT_FALSE(zwo_garage_moving(&O, 0u, NULL));
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_BUSY, zwo_garage_move(&O, NOW, 0u, ZWO_POS_OPEN, NOW, 4u));
    run(20000u);
    TEST_ASSERT_EQUAL_UINT(1u, RESULTS);

    /* no answer from the radio at all: the press times out into the same release */
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_move(&O, NOW, 0u, ZWO_POS_OPEN, NOW, 5u));
    TEST_ASSERT_TRUE(zwo_next_deadline(&O) <= NOW + ZWO_SET_MS);
    sent = N;
    run(ZWO_SET_MS - 200u);
    TEST_ASSERT_EQUAL_UINT(sent, N);
    run(200u);
    TEST_ASSERT_EQUAL_UINT(sent + 1u, N);
    TEST_ASSERT_EQUAL_UINT(1u, RESULTS);
    release_confirmed();
    TEST_ASSERT_EQUAL_UINT(2u, RESULTS);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_TIMEOUT, RES);
    TEST_ASSERT_EQUAL_UINT32(5u, RES_CMD);
    run(20000u);

    /* its off not delivered either: tried ZWO_OFF_TRIES times, then stuck - once */
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_move(&O, NOW, 0u, ZWO_POS_OPEN, NOW, 6u));
    sent = N;
    done(false);
    for (unsigned i = 0; i < ZWO_OFF_TRIES; i++) {
        TEST_ASSERT_EQUAL_UINT(sent + 1u + i, N);
        TEST_ASSERT_EQUAL_HEX8(0x00u, last()[2]);
        TEST_ASSERT_EQUAL_UINT(2u, RESULTS);
        done(false);
    }
    TEST_ASSERT_EQUAL_UINT(sent + ZWO_OFF_TRIES, N);
    TEST_ASSERT_EQUAL_UINT(3u, RESULTS);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_RELAY_STUCK, RES);
    TEST_ASSERT_EQUAL_UINT32(6u, RES_CMD);
    done(false); /* a late answer: nothing more */
    run(60000u);
    TEST_ASSERT_EQUAL_UINT(3u, RESULTS);

    /* the link drops with the press out: the radio refuses the off - no try, sent again
     * ZWO_SET_MS later; the off sent once the link is back */
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_move(&O, NOW, 0u, ZWO_POS_OPEN, NOW, 7u));
    sent = N;
    SEND_FAILS = true;
    done(false);
    TEST_ASSERT_EQUAL_UINT(1u, REFUSED);
    run(ZWO_SET_MS - 100u);
    TEST_ASSERT_EQUAL_UINT(1u, REFUSED);
    run(100u);
    TEST_ASSERT_EQUAL_UINT(2u, REFUSED);
    TEST_ASSERT_EQUAL_UINT(3u, RESULTS);
    SEND_FAILS = false;
    run(ZWO_SET_MS);
    TEST_ASSERT_EQUAL_UINT(sent + 1u, N);
    release_confirmed();
    TEST_ASSERT_EQUAL_UINT(4u, RESULTS);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_SEND, RES);
    TEST_ASSERT_EQUAL_UINT32(7u, RES_CMD);
    run(20000u);
    /* ... and a link that stays down: stuck after ZWO_OFF_REFUSALS refusals, once */
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_move(&O, NOW, 0u, ZWO_POS_OPEN, NOW, 8u));
    SEND_FAILS = true;
    REFUSED = 0u;
    done(false);
    run((uint64_t)(ZWO_OFF_REFUSALS - 1u) * ZWO_SET_MS);
    TEST_ASSERT_EQUAL_UINT(ZWO_OFF_REFUSALS, REFUSED);
    TEST_ASSERT_EQUAL_UINT(4u, RESULTS);
    run(ZWO_SET_MS);
    TEST_ASSERT_EQUAL_UINT(ZWO_OFF_REFUSALS, REFUSED);
    TEST_ASSERT_EQUAL_UINT(5u, RESULTS);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_RELAY_STUCK, RES);
    TEST_ASSERT_EQUAL_UINT32(8u, RES_CMD);
    run(60000u);
    TEST_ASSERT_EQUAL_UINT(5u, RESULTS);
    SEND_FAILS = false;
    /* once the link is back, its relay's report of on is released: no press holds it */
    sent = N;
    level_report(6u, 1u, 0x25u, 0xFFu);
    TEST_ASSERT_EQUAL_UINT(sent + 1u, N);
    TEST_ASSERT_EQUAL_HEX8(0x00u, last()[2]);
}

/* PL03 (ZW-035): a garage is not cleared while its move is under way - pressed, held, released
 * or travelling - so the job that lets its relay go is never dropped; once it has ended, it is. */
static void test_garage_clear_in_flight(void)
{
    const size_t relay = garage_setup();
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_move(&O, NOW, 0u, ZWO_POS_OPEN, NOW, 3u));
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_BUSY, zwo_garage_clear(&O, 0u)); /* PRESS */
    done(true);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_BUSY, zwo_garage_clear(&O, 0u)); /* HOLD */
    const size_t sent = N;
    run(ZWO_PULSE_MS);
    TEST_ASSERT_EQUAL_UINT(sent + 1u, N);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_BUSY, zwo_garage_clear(&O, 0u)); /* RELEASE */
    release_confirmed();
    TEST_ASSERT_EQUAL_INT(ZWO_G_TRAVEL, O.garage[0].state);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_BUSY, zwo_garage_clear(&O, 0u)); /* TRAVEL */
    TEST_ASSERT_TRUE(O.garage[0].used);
    TEST_ASSERT_TRUE(O.out[relay].garage);
    TEST_ASSERT_EQUAL_UINT(0u, RESULTS);
    POS = ZWO_POS_OPEN;
    run(1000u);
    TEST_ASSERT_EQUAL_UINT(1u, RESULTS);
    TEST_ASSERT_EQUAL_INT(ZWO_OK, RES);
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_clear(&O, 0u));
    TEST_ASSERT_FALSE(O.garage[0].used);
    TEST_ASSERT_FALSE(O.out[relay].garage);
    TEST_ASSERT_EQUAL_UINT(1u, RESULTS);

    /* cleared during an unconfirmed press's release: refused too */
    POS = ZWO_POS_CLOSED;
    run(20000u);
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_assign(&O, 0u, relay, 12u, 20u));
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_move(&O, NOW, 0u, ZWO_POS_OPEN, NOW, 4u));
    done(false);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_BUSY, zwo_garage_clear(&O, 0u));
    release_confirmed();
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_SEND, RES);
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_clear(&O, 0u));
}

/* PL02 (ZW-034): a command's answer that comes late - after its own end - never ends the next
 * one: each command ends once, by its own result. */
static void test_late_answer(void)
{
    static const uint8_t SW[] = {0x25};
    record(4u, 1u, SW, sizeof SW, false);
    (void)zwo_scan(&O, &REC);
    const size_t sw = (size_t)zwo_find(&O, 4u, 0u);
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_set(&O, NOW, sw, 100, NOW, 21u));
    const uint32_t first = SENT[N - 1u].tag;
    run(ZWO_SET_MS);
    TEST_ASSERT_EQUAL_UINT(1u, RESULTS);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_TIMEOUT, RES);
    TEST_ASSERT_EQUAL_UINT32(21u, RES_CMD);
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_set(&O, NOW, sw, 0, NOW, 22u));
    TEST_ASSERT_TRUE(zwo_send_done(&O, NOW, first, false)); /* the first one's, late */
    TEST_ASSERT_TRUE(zwo_send_done(&O, NOW, first, true));
    TEST_ASSERT_EQUAL_UINT(1u, RESULTS);
    TEST_ASSERT_EQUAL_UINT32(22u, O.out[sw].cmd);
    done(true);
    level_report(4u, 0u, 0x25u, 0x00u);
    TEST_ASSERT_EQUAL_UINT(2u, RESULTS);
    TEST_ASSERT_EQUAL_INT(ZWO_OK, RES);
    TEST_ASSERT_EQUAL_UINT32(22u, RES_CMD);
}

/* ZW-035: a garage's relay found on with no move under way (a restart during a press) is
 * released at once; during a move it is left to the press. */
static void test_garage_relay_found_on(void)
{
    (void)garage_setup();
    const size_t before = N;
    level_report(6u, 1u, 0x25u, 0xFFu);
    TEST_ASSERT_EQUAL_UINT(before + 1u, N);
    const uint8_t off[] = {0x25, 0x01, 0x00};
    TEST_ASSERT_EQUAL_MEMORY(off, last(), 3u);
    TEST_ASSERT_EQUAL_UINT8(1u, SENT[N - 1u].ep);
    level_report(6u, 1u, 0x25u, 0x00u);
    /* its own press: not released early */
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_move(&O, NOW, 0u, ZWO_POS_OPEN, NOW, 3u));
    done(true);
    const size_t pressing = N;
    level_report(6u, 1u, 0x25u, 0xFFu);
    TEST_ASSERT_EQUAL_UINT(pressing, N);
    run(ZWO_PULSE_MS);
    TEST_ASSERT_EQUAL_UINT(pressing + 1u, N); /* released by the press itself, on time */
}

/* ZW-035: the garage is stored by its relay's device and generation; a relay gone or reused by
 * another device drops it; damage is refused. */
static void test_garage_store(void)
{
    (void)garage_setup();
    uint8_t blob[ZWO_GARAGE_BLOB];
    TEST_ASSERT_EQUAL_UINT(ZWO_GARAGE_BLOB, zwo_garage_encode(&O, blob, sizeof blob));
    zwo_garage_clear(&O, 0u);
    TEST_ASSERT_FALSE(O.garage[0].used);
    TEST_ASSERT_TRUE(zwo_garage_decode(&O, blob, sizeof blob));
    TEST_ASSERT_TRUE(O.garage[0].used);
    TEST_ASSERT_EQUAL_UINT16(12u, O.garage[0].zone);
    TEST_ASSERT_EQUAL_UINT16(20u, O.garage[0].travel_s);
    TEST_ASSERT_TRUE(O.out[O.garage[0].output].garage);
    blob[5] ^= 1u;
    TEST_ASSERT_FALSE(zwo_garage_decode(&O, blob, sizeof blob));
    blob[5] ^= 1u;
    static const uint8_t MULTI[] = {0x5E, 0x25, 0x60};
    record(6u, 2u, MULTI, sizeof MULTI, true); /* another device under node 6 */
    (void)zwo_scan(&O, &REC);
    TEST_ASSERT_FALSE(O.garage[0].used);
    TEST_ASSERT_TRUE(zwo_garage_decode(&O, blob, sizeof blob));
    TEST_ASSERT_FALSE(O.garage[0].used);
    /* a dimmer can't be a garage relay; one relay, one garage */
    static const uint8_t DIM[] = {0x26};
    record(5u, 1u, DIM, sizeof DIM, false);
    (void)zwo_scan(&O, &REC);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_ARG,
                          zwo_garage_assign(&O, 0u, (size_t)zwo_find(&O, 5u, 0u), 12u, 20u));
    const size_t relay = (size_t)zwo_find(&O, 6u, 2u);
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_assign(&O, 0u, relay, 12u, 0u));
    TEST_ASSERT_EQUAL_UINT16(ZWO_TRAVEL_S_DEF, O.garage[0].travel_s);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_BUSY, zwo_garage_assign(&O, 1u, relay, 12u, 0u));
}

/* PL01 (ZW-035, review 2): a press whose on the radio hasn't ended (its queue full on a busy
 * network) times out; an off the radio then refuses is no try - it goes again ZWO_SET_MS later,
 * and at once when the on ends - so an on sent late never finds the release already given up (it
 * was: three refusals ZWO_VERIFY_MS apart ended it RELAY_STUCK 4.5 s on). Only offs the radio
 * took count toward ZWO_OFF_TRIES; refusals are bounded on their own. One result each. */
static void test_garage_off_refused_while_on_out(void)
{
    const size_t relay = garage_setup();
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_move(&O, NOW, 0u, ZWO_POS_OPEN, NOW, 3u));
    uint32_t on = SENT[N - 1u].tag;
    size_t sent = N;
    SEND_FAILS = true; /* the radio's queue is full, the on still in it */
    run(ZWO_SET_MS);   /* no end of the on: the press times out, its off refused */
    TEST_ASSERT_EQUAL_UINT(1u, REFUSED);
    run(3u * ZWO_SET_MS - 100u); /* 30 s on, far past the old 4.5 s: still releasing */
    TEST_ASSERT_EQUAL_UINT(3u, REFUSED);
    TEST_ASSERT_EQUAL_UINT(0u, RESULTS);
    TEST_ASSERT_TRUE(zwo_garage_moving(&O, 0u, NULL));
    TEST_ASSERT_EQUAL_UINT(sent, N);
    /* the on goes out at last (and switches the relay): the queue has room - the off goes now */
    SEND_FAILS = false;
    TEST_ASSERT_TRUE(zwo_send_done(&O, NOW, on, true));
    TEST_ASSERT_EQUAL_UINT(sent + 1u, N);
    level_report(6u, 1u, 0x25u, 0xFFu);
    TEST_ASSERT_EQUAL_UINT(sent + 1u, N); /* its own move releases it: nothing extra */
    release_confirmed();
    TEST_ASSERT_EQUAL_UINT(1u, RESULTS);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_TIMEOUT, RES); /* the press unconfirmed: its outcome unknown */
    TEST_ASSERT_EQUAL_UINT32(3u, RES_CMD);
    TEST_ASSERT_EQUAL_INT8(0, O.out[relay].level);

    /* an on the radio never ends, a queue that never frees: bounded all the same - stuck after
     * ZWO_OFF_REFUSALS refusals, once */
    run(20000u);
    REFUSED = 0u;
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_move(&O, NOW, 0u, ZWO_POS_OPEN, NOW, 4u));
    sent = N;
    SEND_FAILS = true;
    run(ZWO_SET_MS);
    run((uint64_t)(ZWO_OFF_REFUSALS - 1u) * ZWO_SET_MS);
    TEST_ASSERT_EQUAL_UINT(ZWO_OFF_REFUSALS, REFUSED);
    TEST_ASSERT_EQUAL_UINT(1u, RESULTS);
    run(ZWO_SET_MS);
    TEST_ASSERT_EQUAL_UINT(2u, RESULTS);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_RELAY_STUCK, RES);
    TEST_ASSERT_EQUAL_UINT32(4u, RES_CMD);
    TEST_ASSERT_EQUAL_UINT(sent, N);
    run(60000u);
    TEST_ASSERT_EQUAL_UINT(ZWO_OFF_REFUSALS, REFUSED);
    TEST_ASSERT_EQUAL_UINT(2u, RESULTS);
    SEND_FAILS = false;

    /* refusals, then offs the radio takes but the relay never confirms: ZWO_OFF_TRIES of those
     * - the refusals not among them - then stuck, once; the on's late end changes nothing */
    run(20000u);
    REFUSED = 0u;
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_move(&O, NOW, 0u, ZWO_POS_OPEN, NOW, 5u));
    on = SENT[N - 1u].tag;
    SEND_FAILS = true;
    run(2u * ZWO_SET_MS);
    TEST_ASSERT_EQUAL_UINT(2u, REFUSED);
    SEND_FAILS = false;
    sent = N;
    run(ZWO_SET_MS);
    for (unsigned i = 0; i < ZWO_OFF_TRIES; i++) {
        TEST_ASSERT_EQUAL_UINT(sent + 1u + i, N);
        TEST_ASSERT_EQUAL_HEX8(0x00u, last()[2]);
        TEST_ASSERT_EQUAL_UINT(2u, RESULTS);
        done(false);
    }
    TEST_ASSERT_EQUAL_UINT(sent + ZWO_OFF_TRIES, N);
    TEST_ASSERT_EQUAL_UINT(3u, RESULTS);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_RELAY_STUCK, RES);
    TEST_ASSERT_EQUAL_UINT32(5u, RES_CMD);
    TEST_ASSERT_TRUE(zwo_send_done(&O, NOW, on, true));
    run(60000u);
    TEST_ASSERT_EQUAL_UINT(sent + ZWO_OFF_TRIES, N);
    TEST_ASSERT_EQUAL_UINT(3u, RESULTS);
}

/* HCTL-012, AUT-009 (ZW-035): a garage move's whole length - press, pulse, every off it may try
 * or have refused, its travel time - is what its caller waits for (zwo_garage_budget_ms): at the
 * longest travel a slow door that gets there ends done within it, and the slowest release before
 * a door that never gets there ends failed within it, each once. */
static void test_garage_budget(void)
{
    const size_t relay = garage_setup();
    TEST_ASSERT_EQUAL_UINT32(0u, zwo_garage_budget_ms(&O, 1u)); /* no such garage */
    TEST_ASSERT_EQUAL_UINT32(10000u + 1000u + 3u * 13000u + 6u * 10000u + 20000u + 2000u,
                             zwo_garage_budget_ms(&O, 0u));
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_assign(&O, 0u, relay, 12u, ZWO_TRAVEL_S_MAX));
    const uint32_t budget = zwo_garage_budget_ms(&O, 0u);
    TEST_ASSERT_EQUAL_UINT32(232000u, budget);

    /* a slow door: its sensor says closed just before its travel time is out */
    POS = ZWO_POS_OPEN;
    uint64_t t0 = NOW;
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_move(&O, NOW, 0u, ZWO_POS_CLOSED, NOW, 3u));
    press_and_release();
    run(ZWO_TRAVEL_S_MAX * 1000u - 2000u);
    TEST_ASSERT_EQUAL_UINT(0u, RESULTS);
    POS = ZWO_POS_CLOSED;
    run(1000u);
    TEST_ASSERT_EQUAL_UINT(1u, RESULTS);
    TEST_ASSERT_EQUAL_INT(ZWO_OK, RES);
    TEST_ASSERT_TRUE(NOW - t0 <= budget);

    /* the slowest path: the on's end just inside ZWO_SET_MS, its pulse, five refused offs, two
     * offs taken and never confirmed, the third confirmed at the last moment, then a door that
     * never gets there */
    t0 = NOW;
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_move(&O, NOW, 0u, ZWO_POS_OPEN, NOW, 4u));
    run(ZWO_SET_MS - 100u);
    done(true);
    SEND_FAILS = true;
    run(ZWO_PULSE_MS);
    run((uint64_t)(ZWO_OFF_REFUSALS - 2u) * ZWO_SET_MS);
    TEST_ASSERT_EQUAL_UINT(ZWO_OFF_REFUSALS - 1u, REFUSED);
    SEND_FAILS = false;
    run(ZWO_SET_MS);
    for (unsigned i = 0; i < ZWO_OFF_TRIES; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x00u, last()[2]);
        run(ZWO_SET_MS - 100u);
        done(true);
        run(ZWO_VERIFY_MS); /* its Get */
        if (i + 1u < ZWO_OFF_TRIES) {
            run(ZWO_VERIFY_MS);
        } else {
            run(ZWO_VERIFY_MS - 100u);
            level_report(6u, 1u, 0x25u, 0x00u);
        }
    }
    TEST_ASSERT_EQUAL_UINT(1u, RESULTS);
    run(ZWO_TRAVEL_S_MAX * 1000u + 1000u);
    TEST_ASSERT_EQUAL_UINT(2u, RESULTS);
    TEST_ASSERT_EQUAL_INT(ZWO_ERR_NO_MOVE, RES);
    TEST_ASSERT_EQUAL_UINT32(4u, RES_CMD);
    /* it ended within its budget: the last poll's second over it at most */
    TEST_ASSERT_TRUE(NOW - t0 <= (uint64_t)budget + 1000u);
}

/* HCTL-012: a started command's end in its caller's words - done; a door that didn't get there
 * definite; everything else once sent (unacknowledged, unconfirmed, a relay that won't let go,
 * dropped with its device) may have happened: "timeout". */
static void test_end_word(void)
{
    TEST_ASSERT_EQUAL_STRING("", zwo_end_word(ZWO_OK));
    TEST_ASSERT_EQUAL_STRING("failed", zwo_end_word(ZWO_ERR_NO_MOVE));
    static const zwo_result_t UNSURE[] = {ZWO_ERR_SEND, ZWO_ERR_TIMEOUT, ZWO_ERR_RELAY_STUCK,
                                          ZWO_ERR_UNKNOWN};
    for (size_t i = 0; i < sizeof UNSURE / sizeof UNSURE[0]; i++) {
        TEST_ASSERT_EQUAL_STRING("timeout", zwo_end_word(UNSURE[i]));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_scan);
    RUN_TEST(test_set);
    RUN_TEST(test_garage_moves);
    RUN_TEST(test_garage_relay_faults);
    RUN_TEST(test_garage_store);
    RUN_TEST(test_garage_relay_found_on);
    RUN_TEST(test_garage_press_unconfirmed);
    RUN_TEST(test_garage_clear_in_flight);
    RUN_TEST(test_late_answer);
    RUN_TEST(test_garage_off_refused_while_on_out);
    RUN_TEST(test_garage_budget);
    RUN_TEST(test_end_word);
    return UNITY_END();
}
