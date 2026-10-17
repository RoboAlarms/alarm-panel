/*
 * test_plink_fw.c - the master's keypad-update planner (plink_fw.h; docs/features/11 "Updates
 * through the link": The rules, The master).
 *
 * The planner is a pure function: each test builds a world (the master's target and confirmed
 * version, its image, its server and key, the keypads as the link's core reports them), steps it
 * as the service does (its memory kept, its orders numbered and held outstanding as the core
 * holds them) and writes each keypad's answers by hand.
 *
 * Proves, rule by rule: INFO at each session and on a change of the confirmed version, before
 * anything else; one order at a time; FETCH to every keypad behind at once, naming the master's
 * server, channel and key; RELAY when the fetch could not be done (1-9, 20, 21, 13-15), or at once
 * to a keypad that can't fetch, only of an image that fits it (device, chip range, accepted
 * project, slot); a copy for another model, one at a time; a refusal waits for the next version,
 * a failed download is tried again after an hour or when V changes; INSTALL to every READY keypad
 * with the master's own restart, and at once when the master runs V confirmed, once a session; a
 * relay the core no longer serves sent again; a keypad that went back on V left alone until the
 * next version (and asked again from the console); never older or equal, nothing without the FW
 * bit or off line; the rows for the screens.
 *
 * Requirements: LINK-029, LINK-030, LINK-031, LINK-035 (docs/features/11)
 */
#include <stdio.h>
#include <string.h>

#include "panel_link/plink_fw.h"
#include "unity.h"

#define OLD "1.3.0"
#define V   "1.4.0"
#define W   "1.5.0"
#define P4  "crowpanel_p4"
#define P10 "crowpanel_10"
#define PRJ "roboalarms-p4-release"
#define HOUR 3600000u

static plink_fw_world_t WD;
static plink_fw_step_t S;
static uint64_t NOW;
static uint32_t NEXT_ID;

void setUp(void)
{
    memset(&WD, 0, sizeof WD);
    memset(&S, 0, sizeof S);
    NOW = 1000000u;
    NEXT_ID = 0x5000u;
    snprintf(WD.target, sizeof WD.target, "%s", V);
    snprintf(WD.running, sizeof WD.running, "%s", OLD);
    snprintf(WD.server, sizeof WD.server, "https://firmware.example.org");
    snprintf(WD.channel, sizeof WD.channel, "stable");
    memset(WD.key, 0x4B, sizeof WD.key);
    /* its own update, downloaded and checked: the spare slot */
    plink_fw_image_t *im = &WD.own;
    im->src = PLINK_FWSRC_SPARE;
    snprintf(im->version, sizeof im->version, "%s", V);
    snprintf(im->device, sizeof im->device, P4);
    snprintf(im->project, sizeof im->project, PRJ);
    im->chip_min = 0u;
    im->chip_max = 199u;
    im->size = 1200001u;
    memset(im->sha256, 0xA1, sizeof im->sha256);
}

void tearDown(void)
{
}

static plink_fw_kp_t *K(uint8_t d)
{
    return &WD.kp[d - 1u];
}

/* Keypad d on version, online in its first session, its facts and an idle state of this session
 * in (the INFO of this session already echoed when the master's version is known). */
static plink_fw_kp_t *kp_on(uint8_t d, const char *version, uint8_t can)
{
    plink_fw_kp_t *k = K(d);
    memset(k, 0, sizeof *k);
    k->device = d;
    k->fw_cap = k->fw = k->online = true;
    k->sessions = 1u;
    k->have_info = k->info_now = true;
    k->info.can = can;
    snprintf(k->info.device, sizeof k->info.device, P4);
    snprintf(k->info.chip_rev, sizeof k->info.chip_rev, "v1");
    k->info.chip_rev_full = 100u;
    snprintf(k->info.project, sizeof k->info.project, PRJ);
    snprintf(k->info.version, sizeof k->info.version, "%s", version);
    k->info.slot = 4718592u;
    k->have_state = k->state_now = true;
    plink_fw_mem_t *m = &WD.mem[d - 1u];
    m->told_sid = 1u;
    snprintf(m->told, sizeof m->told, "%s", WD.running);
    return k;
}

/* One step as the service runs it: its memory kept, each order handed to the core (numbered,
 * outstanding until echoed; a RELAY served from then on). */
static void step(void)
{
    plink_fw_plan(&WD, NOW, &S);
    memcpy(WD.mem, S.mem, sizeof WD.mem);
    for (uint8_t d = 1u; d <= PLINK_KEYPADS_MAX; d++) {
        const plink_fw_order_t *o = &S.send[d - 1u];
        if (o->op == 0u) {
            continue;
        }
        plink_fw_kp_t *k = K(d);
        k->order = ++NEXT_ID;
        k->order_op = o->op;
        k->order_acked = false;
        k->order_why = 0u;
        if (o->op == PLINK_FW_OP_RELAY) {
            k->relay = true;
            memcpy(k->relay_id8, o->sha256, PLINK_ID8_LEN);
        } else if (o->op != PLINK_FW_OP_INFO) {
            k->relay = false;
        }
    }
}

/* Keypad d answers its newest order: its FW_STATE now in state with why (a refusal: the state it
 * was in, why the refusal), its run for target. */
static void echo(uint8_t d, uint8_t state, uint8_t why, const char *target)
{
    plink_fw_kp_t *k = K(d);
    k->order_acked = true;
    k->order_why = why;
    k->state.order = k->order;
    k->state.op = k->order_op;
    k->state.state = state;
    k->state.why = why;
    snprintf(k->state.target, sizeof k->state.target, "%s", target);
}

/* Its run moves on (a FW_STATE after the echo, the same order). */
static void now_in(uint8_t d, uint8_t state, uint8_t why)
{
    K(d)->state.state = state;
    K(d)->state.why = why;
}

static uint8_t op_to(uint8_t d)
{
    return S.send[d - 1u].op;
}

static uint8_t row_of(uint8_t d)
{
    return S.row[d - 1u].state;
}

/* ---- INFO and one order at a time ---- */

/* LINK-029: INFO names the master's confirmed version at each session and when it changes, before
 * anything else in that session; none while it is on its self-test; one order at a time - nothing
 * new while one is outstanding. */
static void test_info_first_each_session_and_on_change(void)
{
    kp_on(1u, OLD, PLINK_FWI_FETCH | PLINK_FWI_RELAY);
    memset(&WD.mem[0], 0, sizeof WD.mem[0]); /* nothing told yet */
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, op_to(1u));
    TEST_ASSERT_EQUAL_STRING(OLD, S.send[0].version);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_KIND_APP, S.send[0].kind);
    step(); /* unanswered: nothing new */
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(1u));
    echo(1u, PLINK_FWS_IDLE, 0u, "");
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_FETCH, op_to(1u)); /* then the work */
    echo(1u, PLINK_FWS_FETCHING, 0u, V);
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(1u));
    /* A new session: INFO again first */
    K(1u)->sessions++;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, op_to(1u));
    echo(1u, PLINK_FWS_FETCHING, 0u, V);
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(1u));
    /* The master's self-test (no confirmed version): no INFO; then confirmed on V: INFO{V} */
    WD.running[0] = '\0';
    K(1u)->sessions++;
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(1u));
    snprintf(WD.running, sizeof WD.running, "%s", V);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, op_to(1u));
    TEST_ASSERT_EQUAL_STRING(V, S.send[0].version);
}

/* ---- getting the image ---- */

/* LINK-030, LINK-036: FETCH{V} goes to every keypad behind at once, naming the master's server,
 * channel and key; a keypad on V or newer gets nothing (never older or equal), nor one without
 * the FW bit (or on frames under 250 B) or not heard for 30 s. */
static void test_fetch_to_every_keypad_behind_at_once(void)
{
    for (uint8_t d = 1u; d <= 3u; d++) {
        kp_on(d, OLD, PLINK_FWI_FETCH | PLINK_FWI_RELAY);
    }
    kp_on(4u, V, PLINK_FWI_FETCH | PLINK_FWI_RELAY);
    kp_on(5u, W, PLINK_FWI_FETCH | PLINK_FWI_RELAY);
    kp_on(6u, OLD, PLINK_FWI_FETCH | PLINK_FWI_RELAY)->fw = false;
    K(6u)->fw_cap = false;
    kp_on(7u, OLD, PLINK_FWI_FETCH | PLINK_FWI_RELAY)->online = false;
    step();
    for (uint8_t d = 1u; d <= 3u; d++) {
        const plink_fw_order_t *o = &S.send[d - 1u];
        TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_FETCH, o->op);
        TEST_ASSERT_EQUAL_STRING(V, o->version);
        TEST_ASSERT_EQUAL_STRING("https://firmware.example.org", o->server);
        TEST_ASSERT_EQUAL_STRING("stable", o->channel);
        TEST_ASSERT_EQUAL_HEX8(0x4Bu, o->key[0]);
        TEST_ASSERT_EQUAL_HEX8(0x4Bu, o->key[PLINK_FW_KEY_LEN - 1u]);
        TEST_ASSERT_EQUAL_UINT8(PLINK_FWR_DOWNLOADING, row_of(d));
    }
    for (uint8_t d = 4u; d <= 7u; d++) {
        TEST_ASSERT_EQUAL_UINT8(0u, op_to(d));
    }
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWR_UP_TO_DATE, row_of(4u));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWR_NEWER, row_of(5u));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWR_NONE, row_of(6u));
    /* No target: nothing at all */
    WD.target[0] = '\0';
    kp_on(1u, OLD, PLINK_FWI_FETCH);
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(1u));
}

/* LINK-031: a FETCH that could not be done goes to the relay at once (not reached 1-9, broke off
 * 20, too slow 21, another version, nothing newer or no release of V 13-15), from the master's
 * image of V, with its size, SHA-256, page and count; the server's refusals (10-12, 16-19) wait for
 * the next version, any other failure an hour. */
static void test_relay_when_the_fetch_could_not_be_done(void)
{
    static const struct {
        uint8_t why;
        uint8_t then; /* RELAY, or 0: none - row NOT_UPDATED */
        bool hour;    /* ... tried again in an hour (else at the next version) */
    } T[] = {
        {PLINK_FW_WHY_NO_NETWORK, PLINK_FW_OP_RELAY, false},
        {PLINK_FW_WHY_NOT_A_SERVER, PLINK_FW_OP_RELAY, false},
        {PLINK_FW_WHY_BROKE_OFF, PLINK_FW_OP_RELAY, false},
        {PLINK_FW_WHY_TOO_SLOW, PLINK_FW_OP_RELAY, false},
        {PLINK_FW_WHY_OTHER_VERSION, PLINK_FW_OP_RELAY, false},
        {PLINK_FW_WHY_UP_TO_DATE, PLINK_FW_OP_RELAY, false},
        {PLINK_FW_WHY_UNKNOWN_VERSION, PLINK_FW_OP_RELAY, false},
        {PLINK_FW_WHY_UNKNOWN_DEVICE, 0u, false},
        {PLINK_FW_WHY_HELD_BACK, 0u, false},
        {PLINK_FW_WHY_STEPPING_STONE, 0u, false},
        {PLINK_FW_WHY_NOT_OFFERED, 0u, false},
        {17u, 0u, false},
        {19u, 0u, false},
        {PLINK_FW_WHY_BAD_SHA, 0u, true},
        {PLINK_FW_WHY_FLASH, 0u, true},
        {PLINK_FW_WHY_SIGNATURE, 0u, false},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        setUp();
        kp_on(1u, OLD, PLINK_FWI_FETCH | PLINK_FWI_RELAY);
        step();
        TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_FETCH, op_to(1u));
        echo(1u, PLINK_FWS_FETCHING, 0u, V);
        now_in(1u, PLINK_FWS_FAILED, T[i].why);
        step();
        char msg[32];
        snprintf(msg, sizeof msg, "why %u", (unsigned)T[i].why);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(T[i].then, op_to(1u), msg);
        if (T[i].then == PLINK_FW_OP_RELAY) {
            const plink_fw_order_t *o = &S.send[0];
            TEST_ASSERT_EQUAL_STRING(V, o->version);
            TEST_ASSERT_EQUAL_UINT32(1200001u, o->size);
            TEST_ASSERT_EQUAL_HEX8(0xA1u, o->sha256[0]);
            TEST_ASSERT_EQUAL_UINT16(PLINK_FWP_PAGE, o->page);
            TEST_ASSERT_EQUAL_UINT8(PLINK_FWP_COUNT, o->count);
            TEST_ASSERT_EQUAL_STRING("https://firmware.example.org", o->server);
            TEST_ASSERT_EQUAL_UINT8(PLINK_FWR_RECEIVING, row_of(1u));
            continue;
        }
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(PLINK_FWR_NOT_UPDATED, row_of(1u), msg);
        TEST_ASSERT_EQUAL_UINT8(T[i].why, S.row[0].why);
        NOW += HOUR;
        step();
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(T[i].hour ? PLINK_FW_OP_FETCH : 0u, op_to(1u), msg);
        NOW += 30u * 24u * HOUR;
        step();
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, op_to(1u), msg);
    }
}

/* LINK-031: a keypad that can't fetch gets the relay at once; refused orders count as failures
 * (BUDGET in an hour, TOO_BIG at the next version). */
static void test_relay_at_once_and_refused_orders(void)
{
    kp_on(1u, OLD, PLINK_FWI_RELAY);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, op_to(1u));
    echo(1u, PLINK_FWS_IDLE, PLINK_FW_WHY_BUDGET, ""); /* refused: 3 failed runs in 24 h */
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(1u));
    NOW += HOUR - 1u;
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(1u));
    NOW += 1u;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, op_to(1u));
    echo(1u, PLINK_FWS_IDLE, PLINK_FW_WHY_TOO_BIG, "");
    step();
    NOW += 30u * 24u * HOUR;
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(1u));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWR_NOT_UPDATED, row_of(1u));
}

/* LINK-031: the master relays only an image that fits the keypad - the same device, within the
 * image's chip revisions, of a project it accepts, within its slot; otherwise it wants a copy with
 * the keypad's facts. */
static void test_relay_only_an_image_that_fits(void)
{
    kp_on(1u, OLD, PLINK_FWI_RELAY);
    snprintf(K(1u)->info.device, sizeof K(1u)->info.device, P10);
    kp_on(2u, OLD, PLINK_FWI_RELAY)->info.chip_rev_full = 300u;
    snprintf(K(2u)->info.chip_rev, sizeof K(2u)->info.chip_rev, "v3");
    kp_on(3u, OLD, PLINK_FWI_RELAY);
    snprintf(K(3u)->info.project, sizeof K(3u)->info.project, "roboalarms-p4-other");
    kp_on(4u, OLD, PLINK_FWI_RELAY)->info.slot = 1000000u;
    kp_on(5u, OLD, PLINK_FWI_RELAY);
    snprintf(K(5u)->info.project, sizeof K(5u)->info.project, "roboalarms-p4-dev");
    step();
    for (uint8_t d = 1u; d <= 4u; d++) {
        TEST_ASSERT_EQUAL_UINT8(0u, op_to(d));
    }
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, op_to(5u)); /* -dev accepts -release */
    /* The copy wanted: keypad 1's facts, the first that needs one */
    TEST_ASSERT_EQUAL_STRING(V, S.copy.version);
    TEST_ASSERT_EQUAL_STRING(P10, S.copy.device);
    TEST_ASSERT_EQUAL_STRING("v1", S.copy.chip_rev);
    TEST_ASSERT_EQUAL_STRING(PRJ, S.copy.project);
}

/* LINK-031 (decision 9): another model without internet gets an image the master downloads for
 * it - one copy at a time, kept while a keypad of its facts still needs it; a copy that fails for
 * a reach reason is tried again in an hour, one the server refuses waits for the next version. */
static void test_copy_for_another_model(void)
{
    kp_on(1u, OLD, PLINK_FWI_FETCH | PLINK_FWI_RELAY);
    snprintf(K(1u)->info.device, sizeof K(1u)->info.device, P10);
    kp_on(2u, OLD, PLINK_FWI_RELAY);
    snprintf(K(2u)->info.chip_rev, sizeof K(2u)->info.chip_rev, "v3");
    K(2u)->info.chip_rev_full = 300u;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_FETCH, op_to(1u));
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(2u));
    TEST_ASSERT_EQUAL_STRING("v3", S.copy.chip_rev); /* keypad 2's, for now */
    echo(1u, PLINK_FWS_FETCHING, 0u, V);
    now_in(1u, PLINK_FWS_FAILED, PLINK_FW_WHY_NO_CONNECT); /* its router */
    /* The service fetches keypad 2's copy */
    WD.copy.state = PLINK_FWCOPY_FETCHING;
    WD.copy.facts = S.copy;
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(1u));
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(2u));
    TEST_ASSERT_EQUAL_STRING("v3", S.copy.chip_rev); /* the one held, while it serves */
    WD.copy.state = PLINK_FWCOPY_READY;
    plink_fw_image_t *c = &WD.copy.image;
    *c = WD.own;
    c->src = PLINK_FWSRC_COPY;
    c->chip_min = 300u;
    c->chip_max = 399u;
    c->size = 1300003u;
    memset(c->sha256, 0xC3, sizeof c->sha256);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, op_to(2u));
    TEST_ASSERT_EQUAL_HEX8(0xC3u, S.send[1].sha256[0]);
    TEST_ASSERT_EQUAL_UINT32(1300003u, S.send[1].size);
    echo(2u, PLINK_FWS_RECEIVING, 0u, V);
    memcpy(K(2u)->state.id8, c->sha256, PLINK_ID8_LEN);
    step();
    TEST_ASSERT_EQUAL_STRING("v3", S.copy.chip_rev); /* kept while it receives */
    now_in(2u, PLINK_FWS_READY, 0u);
    step();
    TEST_ASSERT_EQUAL_STRING(P10, S.copy.device); /* then keypad 1's */
    TEST_ASSERT_EQUAL_STRING("v1", S.copy.chip_rev);
    /* Keypad 1's copy: refused by the server - at the next version */
    memset(&WD.copy, 0, sizeof WD.copy);
    WD.copy.state = PLINK_FWCOPY_FAILED;
    WD.copy.why = PLINK_FW_WHY_UNKNOWN_DEVICE;
    WD.copy.facts = S.copy;
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(1u));
    TEST_ASSERT_EQUAL_STRING("", S.copy.version); /* none wanted: freed */
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWR_NOT_UPDATED, row_of(1u));
    NOW += 30u * 24u * HOUR;
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(1u));
    /* The next version: its FETCH again, its copy for a reach reason - an hour */
    memset(K(2u), 0, sizeof *K(2u)); /* (keypad 2 removed: only keypad 1 wants a copy) */
    snprintf(WD.target, sizeof WD.target, "%s", W);
    snprintf(WD.own.version, sizeof WD.own.version, "%s", W);
    memset(&WD.copy, 0, sizeof WD.copy);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_FETCH, op_to(1u));
    echo(1u, PLINK_FWS_FAILED, PLINK_FW_WHY_NO_CONNECT, W);
    step();
    WD.copy.state = PLINK_FWCOPY_FAILED;
    WD.copy.why = PLINK_FW_WHY_NO_LOOKUP;
    WD.copy.facts = S.copy;
    TEST_ASSERT_EQUAL_STRING(W, S.copy.version);
    step();
    TEST_ASSERT_EQUAL_STRING("", S.copy.version);
    NOW += HOUR;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_FETCH, op_to(1u));
}

/* ---- restarting together ---- */

/* LINK-030, LINK-034: INSTALL goes to every READY keypad once the master's own restart is
 * allowed, all in one step (the service waits for the echoes: installing), once a session; a
 * keypad still downloading isn't told. */
static void test_install_with_the_masters_restart(void)
{
    for (uint8_t d = 1u; d <= 3u; d++) {
        kp_on(d, OLD, PLINK_FWI_FETCH | PLINK_FWI_RELAY);
    }
    step();
    for (uint8_t d = 1u; d <= 3u; d++) {
        echo(d, d == 3u ? PLINK_FWS_FETCHING : PLINK_FWS_READY, 0u, V);
    }
    NOW += 600000u;
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, S.send[0].op | S.send[1].op | S.send[2].op);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWR_READY, row_of(1u));
    WD.own_restart = true;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, op_to(1u));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, op_to(2u));
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(3u));
    TEST_ASSERT_EQUAL_STRING(V, S.send[0].version);
    TEST_ASSERT_EQUAL_HEX8(0x03u, S.installing);
    step();
    TEST_ASSERT_EQUAL_HEX8(0x03u, S.installing); /* not echoed yet */
    echo(1u, PLINK_FWS_READY, 0u, V);
    echo(2u, PLINK_FWS_INSTALLING, 0u, V);
    step();
    TEST_ASSERT_EQUAL_HEX8(0u, S.installing);
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(1u)); /* once a session */
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWR_RESTARTING, row_of(2u));
    /* Keypad 1 never restarted (its gate): in its next session, INSTALL again */
    K(1u)->sessions++;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, op_to(1u));
    echo(1u, PLINK_FWS_READY, 0u, V);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, op_to(1u));
}

/* LINK-030: a keypad found behind while the master already runs V confirmed installs as soon as it
 * is READY (it catches up by itself); not while the master is only installing V. */
static void test_install_at_once_when_the_master_runs_v(void)
{
    kp_on(1u, OLD, PLINK_FWI_RELAY);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, op_to(1u));
    echo(1u, PLINK_FWS_READY, 0u, V);
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(1u)); /* the master runs 1.3.0 */
    snprintf(WD.running, sizeof WD.running, "%s", V);
    WD.own.src = PLINK_FWSRC_RUNNING;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, op_to(1u));
    echo(1u, PLINK_FWS_READY, 0u, V);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, op_to(1u));
    /* Back on V: up to date, nothing more */
    echo(1u, PLINK_FWS_INSTALLING, 0u, V);
    K(1u)->sessions++;
    snprintf(K(1u)->info.version, sizeof K(1u)->info.version, "%s", V);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, op_to(1u));
    echo(1u, PLINK_FWS_IDLE, 0u, "");
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(1u));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWR_UP_TO_DATE, row_of(1u));
}

/* LINK-032: a relay under way whose image the core no longer serves (the master restarted: its
 * memory and the core's are new) is sent again, the same image, after the session's INFO; one
 * served is left alone. */
static void test_relay_sent_again_after_a_master_restart(void)
{
    kp_on(1u, OLD, PLINK_FWI_FETCH | PLINK_FWI_RELAY);
    memset(&WD.mem[0], 0, sizeof WD.mem[0]);
    K(1u)->state.state = PLINK_FWS_RECEIVING;
    K(1u)->state.order = 0x77u; /* the old master's */
    snprintf(K(1u)->state.target, sizeof K(1u)->state.target, V);
    memset(K(1u)->state.id8, 0xA1, PLINK_ID8_LEN);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, op_to(1u));
    echo(1u, PLINK_FWS_RECEIVING, 0u, V);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, op_to(1u));
    TEST_ASSERT_EQUAL_HEX8(0xA1u, S.send[0].sha256[0]);
    echo(1u, PLINK_FWS_RECEIVING, 0u, V);
    K(1u)->state.bytes = 600000u;
    K(1u)->state.size = 1200001u;
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(1u));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWR_RECEIVING, row_of(1u));
    TEST_ASSERT_EQUAL_UINT8(49u, S.row[0].percent);
}

/* ---- failures ---- */

/* LINK-030: a failed download is tried again after an hour (from the start: its own server
 * first), and at once when V changes. */
static void test_failed_download_tried_again_after_an_hour(void)
{
    kp_on(1u, OLD, PLINK_FWI_FETCH | PLINK_FWI_RELAY);
    step();
    echo(1u, PLINK_FWS_FAILED, PLINK_FW_WHY_NO_CONNECT, V);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, op_to(1u));
    echo(1u, PLINK_FWS_RECEIVING, 0u, V);
    now_in(1u, PLINK_FWS_FAILED, PLINK_FW_WHY_STALLED);
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(1u));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWR_NOT_UPDATED, row_of(1u));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_WHY_STALLED, S.row[0].why);
    NOW += HOUR - 1u;
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(1u));
    NOW += 1u;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_FETCH, op_to(1u));
    echo(1u, PLINK_FWS_FAILED, PLINK_FW_WHY_BAD_SHA, V);
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(1u));
    /* V changes: at once */
    snprintf(WD.target, sizeof WD.target, "%s", W);
    snprintf(WD.own.version, sizeof WD.own.version, "%s", W);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_FETCH, op_to(1u));
    TEST_ASSERT_EQUAL_STRING(W, S.send[0].version);
    /* A run it lost (it restarted mid-way: its new session knows none) starts again at once */
    echo(1u, PLINK_FWS_FETCHING, 0u, W);
    K(1u)->sessions++;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, op_to(1u));
    echo(1u, PLINK_FWS_IDLE, 0u, "");
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_FETCH, op_to(1u));
}

/* LINK-030: a keypad that went back on V - its own word (FW_STATE's last section), or back behind
 * V in a later session than its INSTALL without V READY - isn't ordered V again until the target
 * changes, through days; the console's ask again sends it on once more, and its going back after
 * that INSTALL holds it again. */
static void test_went_back_not_ordered_again_until_the_next_version(void)
{
    kp_on(1u, OLD, PLINK_FWI_FETCH | PLINK_FWI_RELAY);
    K(1u)->state.last = PLINK_FW_LAST_ROLLED_BACK;
    snprintf(K(1u)->state.last_version, sizeof K(1u)->state.last_version, V);
    kp_on(2u, OLD, PLINK_FWI_FETCH | PLINK_FWI_RELAY);
    snprintf(WD.running, sizeof WD.running, "%s", V);
    WD.own.src = PLINK_FWSRC_RUNNING;
    memset(WD.mem, 0, sizeof WD.mem);
    step();
    echo(1u, PLINK_FWS_IDLE, 0u, "");
    echo(2u, PLINK_FWS_READY, 0u, V);
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(1u));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWR_NOT_UPDATED, row_of(1u));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, op_to(2u));
    echo(2u, PLINK_FWS_INSTALLING, 0u, V);
    /* Keypad 2 back in a new session, behind V, its image gone, nothing said */
    K(2u)->sessions++;
    step();
    echo(2u, PLINK_FWS_IDLE, 0u, "");
    for (unsigned day = 0; day < 3u; day++) {
        NOW += 24u * HOUR;
        step();
        TEST_ASSERT_EQUAL_UINT8(0u, op_to(1u));
        TEST_ASSERT_EQUAL_UINT8(0u, op_to(2u));
    }
    /* Asked again (the console): keypad 1 is sent on */
    plink_fw_again(&WD.mem[0]);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_FETCH, op_to(1u));
    echo(1u, PLINK_FWS_READY, 0u, V);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, op_to(1u));
    echo(1u, PLINK_FWS_INSTALLING, 0u, V);
    K(1u)->sessions++; /* back on the old version again */
    step();
    echo(1u, PLINK_FWS_IDLE, 0u, "");
    NOW += 24u * HOUR;
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(1u));
    /* The master's next version: both ordered again */
    snprintf(WD.target, sizeof WD.target, "%s", W);
    snprintf(WD.own.version, sizeof WD.own.version, "%s", W);
    WD.own.src = PLINK_FWSRC_SPARE;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_FETCH, op_to(1u));
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_FETCH, op_to(2u));
}

/* ---- the rows ---- */

/* What the master's Software update shows per keypad (docs/features/11 "What people see"). */
static void test_rows(void)
{
    kp_on(1u, OLD, PLINK_FWI_FETCH | PLINK_FWI_RELAY);
    step();
    echo(1u, PLINK_FWS_FETCHING, 0u, V);
    K(1u)->state.bytes = 300000u;
    K(1u)->state.size = 1200000u;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWR_DOWNLOADING, row_of(1u));
    TEST_ASSERT_EQUAL_UINT8(25u, S.row[0].percent);
    now_in(1u, PLINK_FWS_VERIFYING, 0u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWR_DOWNLOADING, row_of(1u));
    now_in(1u, PLINK_FWS_READY, 0u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWR_READY, row_of(1u));
    now_in(1u, PLINK_FWS_INSTALLING, 0u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWR_RESTARTING, row_of(1u));
    /* Off line: the row stays as it was last heard, nothing ordered */
    K(1u)->online = false;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWR_RESTARTING, row_of(1u));
    TEST_ASSERT_EQUAL_UINT8(0u, op_to(1u));
    /* Not heard yet: no row */
    memset(K(2u), 0, sizeof *K(2u));
    K(2u)->device = 2u;
    K(2u)->fw_cap = K(2u)->fw = true;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWR_NONE, row_of(2u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_info_first_each_session_and_on_change);
    RUN_TEST(test_fetch_to_every_keypad_behind_at_once);
    RUN_TEST(test_relay_when_the_fetch_could_not_be_done);
    RUN_TEST(test_relay_at_once_and_refused_orders);
    RUN_TEST(test_relay_only_an_image_that_fits);
    RUN_TEST(test_copy_for_another_model);
    RUN_TEST(test_install_with_the_masters_restart);
    RUN_TEST(test_install_at_once_when_the_master_runs_v);
    RUN_TEST(test_relay_sent_again_after_a_master_restart);
    RUN_TEST(test_failed_download_tried_again_after_an_hour);
    RUN_TEST(test_went_back_not_ordered_again_until_the_next_version);
    RUN_TEST(test_rows);
    return UNITY_END();
}
