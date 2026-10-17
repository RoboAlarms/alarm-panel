/*
 * test_plink_fw.c - the master's planner for updates through the link (plink_fw.h;
 * docs/features/11 "Updates through the link": "Keeping keypads on the master's version",
 * "Choosing the path, per keypad", "The master").
 *
 * The planner is a pure function: each test builds a world (the master on 1.4.0, confirmed, its
 * running image, its server, the keypads as the link's core would report them) and steps it,
 * keeping the tracks and holds it returns (as the service does, the holds in link/fwgo) and
 * numbering the orders it asks for as the core does; the keypads' FW_STATEs are written by hand,
 * or by a scripted keypad that answers every order (its install rolls back, its relay stalls, or
 * all goes well).
 *
 * Proves: the path table, row by row (PROBE first, asked again when its answer was lost; FETCH for
 * exactly the target; the server's refusals never relayed; UNKNOWN_VERSION from the own slot
 * only; the relay from the own slot - the same device, chip range and accepted project, a size
 * within the slot - else a copy with the keypad's facts, else NO_IMAGE); orders only while the
 * master is confirmed and quiet, to keypads with FW on frames of 250 B, never older; every keypad
 * behind brought to the master's version with no person's step - after the master's own update,
 * and a keypad found behind later (newly paired, back on line, its FW_INFO older); PROBE and FETCH
 * to every keypad at once, one relay and one install at a time in device-number order; the
 * install's flow to UPDATED through the self-test; each hold ending by itself - a stop after 24 h
 * or at the master's next version, a transfer failure after 2 min and after its 24 h budget, a
 * blocked keypad every 24 h, at the master's check and when its facts change, the canary (a
 * rollback, or no return within 10 min) only at the master's next version; Try again now ending
 * the master's holds but never a BUDGET; the holds kept across a master restart (link/fwgo), and a
 * rollback read from a keypad's last section; no keypad restarted in a loop (one that rolls back
 * every time is restarted once per master version, through days of checks and a master restart);
 * a fetch falling back to the relay once, a refusal never; CANCEL on a stop and on the master's own
 * update, none while it merely isn't quiet; runs taken up after a master restart (an INSTALL a
 * keypad holds, and a relay by id8, even before its FW_INFO; neither hidden by another order),
 * and lost ones tried again; one copy at a time and freed when unused. A relay that timed out
 * paused at its gate (armed) is no failed run; frames that go once and are lost - a FW_INFO, a
 * keypad's FW_STATE after its restart, a run's end - are asked for (INFO), never read as a keypad
 * that didn't come back or a run that broke off, nor leave one holding a turn; a run is gone when
 * its keypad says it knows no order and runs nothing, or 30 s heard in a newer session without a
 * FW_STATE there; INFO names the master's version once it is confirmed. A keypad told to install
 * holds its model only on its word that it restarted and went back: an INSTALL echoed and then
 * unheard is NO_RETURN at 10 min (never put back to READY by the echo of before), undone when it
 * turns out never to have restarted; a FW_STATE in a new session showing it never restarted (READY,
 * its image discarded, stopped), a keypad back from away before its FW_STATE, and a restart never
 * seen installing whose last section names no rollback of the target hold nothing. A PROBE answered
 * with a waiting reason is asked again, never relayed; a relay at KEY_WAIT across a master restart
 * gets its RELAY again once its key is accepted. No two keypads restart on INSTALLs at once: an
 * INSTALL given up (NO_RETURN, or 24 h of the master's quiet at its keypad's gate) is voided
 * (INFO, CANCEL) and the turn waits for that while its keypad is heard; a CANCEL refused BUSY goes
 * again once the restart is put off; no INSTALL before a keypad's facts of its session (a stop
 * holds it whatever them); one of two relays taken up at a restart waits. A NO_RETURN undone
 * leaves a hold another keypad went back under; a model's hold ends on news of its passing, and
 * Try again now starts with a keypad heard and behind; the copy is only for a keypad heard, and
 * one larger than its slot blocks it; whether a keypad restarted is judged on that install alone.
 *
 * Requirements: LINK-029, LINK-030, LINK-031, LINK-035 (docs/features/11)
 */
#include <stdio.h>
#include <string.h>

#include "panel_link/plink_fw.h"
#include "unity.h"

#define V       "1.4.0"
#define NEXT    "1.5.0"
#define OLD     "1.3.0"
#define DEVICE  "crowpanel_p4"
#define DEV10   "crowpanel_10"
#define PROJECT "roboalarms-p4-release"
#define SLOT    4718592u
#define MIN_MS  60000u
#define HOUR_MS 3600000u
#define DAY_MS  86400000u

static plink_fw_world_t W;
static plink_fw_step_t S;
static uint64_t NOW;
static uint32_t IDS;

static void str(char *out, size_t cap, const char *in)
{
    memset(out, 0, cap);
    strncpy(out, in, cap - 1u);
}

/* A scripted keypad (model): how its runs end, and what it was told. M_LATE boots the target, its
 * core speaking before its service (probation over, nothing more said), then its self-test fails
 * and it goes back - its service never saying a word of either. */
enum { M_OFF = 0, M_GOOD, M_ROLLBACK, M_STALL, M_LATE };
typedef struct {
    uint8_t mode;
    uint64_t restart_at;   /* it restarts then (0: not) */
    uint64_t back_at;      /* M_LATE: it goes back then (0: not) */
    uint64_t stall_at;     /* its relay stalls then (0: not) */
    uint64_t fails[3];     /* its own budget's failed runs */
    char target[PLINK_FW_VERSION_MAX + 1];
    char prev[PLINK_FW_VERSION_MAX + 1]; /* the version it ran before its restart */
    unsigned ops[8];       /* orders it took, by op */
} model_t;
static model_t M[PLINK_KEYPADS_MAX];

/* The master on V, confirmed and quiet, reaching its server; its running image (v1 chips) worked
 * out; no holds. */
void setUp(void)
{
    memset(&W, 0, sizeof W);
    memset(&S, 0, sizeof S);
    memset(M, 0, sizeof M);
    NOW = 5000000u;
    IDS = 0x7000u;
    W.own = PLINK_FWM_IDLE;
    W.quiet = true;
    W.check_ms = NOW;
    str(W.server, sizeof W.server, "roboalarms.com");
    str(W.channel, sizeof W.channel, "stable");
    plink_fw_image_t *im = &W.run.image;
    str(im->version, sizeof im->version, V);
    str(im->device, sizeof im->device, DEVICE);
    str(im->project, sizeof im->project, PROJECT);
    im->size = 3600000u;
    for (unsigned i = 0; i < PLINK_SHA256_LEN; i++) {
        im->sha256[i] = (uint8_t)(0xA0u + i);
        im->key[i] = (uint8_t)(0x50u + i);
    }
    W.run.have = true;
    W.run.chip_min = 0u;
    W.run.chip_max = 199u;
}

void tearDown(void) {}

static plink_fw_kp_t *kp(uint8_t d)
{
    return &W.kp[d - 1u];
}

static plink_fw_track_t *tr(uint8_t d)
{
    return &W.track[d - 1u];
}

static const plink_fw_order_t *sent(uint8_t d)
{
    return &S.send[d - 1u];
}

/* Keypad d on version, a v1 7" like the master, reachable in a session with FW. */
static plink_fw_kp_t *keypad(uint8_t d, const char *version)
{
    plink_fw_kp_t *k = kp(d);
    memset(k, 0, sizeof *k);
    k->device = d;
    k->fw_cap = k->fw = k->online = true;
    k->sessions = 1u;
    k->have_info = k->info_now = true;
    k->info.can = PLINK_FWI_FETCH | PLINK_FWI_RELAY | PLINK_FWI_KEY_ACCEPT;
    str(k->info.device, sizeof k->info.device, DEVICE);
    str(k->info.chip_rev, sizeof k->info.chip_rev, "v1");
    k->info.chip_rev_full = 100u;
    str(k->info.project, sizeof k->info.project, PROJECT);
    str(k->info.version, sizeof k->info.version, version);
    k->info.slot = SLOT;
    k->info_ms = NOW;
    /* Told the master's version in this session already (the INFO each session brings, LINK-029):
     * a test of that INFO makes a new session, or another version. */
    plink_fw_track_t *t = tr(d);
    memset(t, 0, sizeof *t);
    str(t->target, sizeof t->target, W.run.image.version);
    t->told = k->sessions;
    return k;
}

/* A v3 chip (another model for the canary; outside the image's range). */
static void v3(uint8_t d)
{
    str(kp(d)->info.chip_rev, sizeof kp(d)->info.chip_rev, "v3");
    kp(d)->info.chip_rev_full = 300u;
}

/* One step: its tracks and holds kept (the holds as the service stores them), its orders numbered
 * and held as the core holds them. */
static void step(void)
{
    plink_fw_plan(&W, NOW, &S);
    memcpy(W.track, S.track, sizeof W.track);
    W.holds = S.holds;
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        if (S.send[i].op != 0u) {
            plink_fw_kp_t *k = &W.kp[i];
            k->order = ++IDS;
            k->order_op = S.send[i].op;
            k->order_acked = false;
            k->order_why = 0u;
            k->order_ms = NOW;
        }
    }
}

static unsigned orders(void)
{
    unsigned n = 0u;
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        n += S.send[i].op != 0u ? 1u : 0u;
    }
    return n;
}

/* An order that starts something went to keypad d this step. */
static bool starts(uint8_t d)
{
    const uint8_t op = sent(d)->op;
    return op == PLINK_FW_OP_PROBE || op == PLINK_FW_OP_FETCH || op == PLINK_FW_OP_RELAY ||
           op == PLINK_FW_OP_INSTALL;
}

/* Keypad d's FW_STATE now, about its newest order (the first one echoing it acks it, with why). */
static plink_fw_state_t *says(uint8_t d, uint8_t state, uint8_t why)
{
    plink_fw_kp_t *k = kp(d);
    k->have_state = k->state_now = true;
    k->state.order = k->order;
    k->state.op = k->order_op;
    k->state.state = state;
    k->state.why = why;
    str(k->state.target, sizeof k->state.target, W.run.image.version);
    k->state_ms = NOW;
    if (k->order != 0u && !k->order_acked) {
        k->order_acked = true;
        k->order_why = why;
    }
    return &k->state;
}

static void answer_probe(uint8_t d, uint8_t why, const char *offered)
{
    plink_fw_state_t *s = says(d, PLINK_FWS_IDLE, PLINK_FW_WHY_OK);
    s->asked = 1u;
    s->probe_why = why;
    str(s->offered, sizeof s->offered, offered);
    s->age_s = 0u;
}

/* Keypad d back after a restart: a new session, its FW_INFO on version, and its first FW_STATE
 * (state_now false: none yet in that session). */
static void back(uint8_t d, const char *version, bool state_now, uint8_t probation, uint8_t last)
{
    plink_fw_kp_t *k = kp(d);
    k->sessions++;
    k->online = true;
    k->away = false;
    k->info_now = true;
    str(k->info.version, sizeof k->info.version, version);
    k->state_now = state_now;
    if (state_now) {
        memset(&k->state, 0, sizeof k->state);
        k->have_state = true;
        k->state.probation = probation;
        k->state.last = last;
        str(k->state.last_version, sizeof k->state.last_version, W.run.image.version);
        k->state_ms = NOW;
    }
}

static void expect(uint8_t d, uint8_t state, uint8_t why)
{
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(state, tr(d)->state, "state");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(why, tr(d)->why, "why");
}

/* Keypad d from BEHIND through PROBE to its server's answer (why, offered). */
static void probed(uint8_t d, uint8_t why, const char *offered)
{
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(d)->op);
    says(d, PLINK_FWS_PROBING, PLINK_FW_WHY_OK);
    NOW += 1500u;
    answer_probe(d, why, offered);
    step();
}

/* Keypad d's run to READY (whatever path it was ordered on). */
static void ready(uint8_t d)
{
    says(d, PLINK_FWS_READY, PLINK_FW_WHY_OK);
}

/* Keypads 1..n (made already) PROBEd at once, offered exactly the target, told to FETCH. */
static void all_fetching(uint8_t n)
{
    step();
    for (uint8_t d = 1u; d <= n; d++) {
        TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(d)->op);
        says(d, PLINK_FWS_PROBING, PLINK_FW_WHY_OK);
    }
    NOW += 1500u;
    for (uint8_t d = 1u; d <= n; d++) {
        answer_probe(d, PLINK_FW_WHY_OK, W.run.image.version);
    }
    step();
    for (uint8_t d = 1u; d <= n; d++) {
        TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_FETCH, sent(d)->op);
        says(d, PLINK_FWS_FETCHING, PLINK_FW_WHY_OK);
    }
}

/* Keypad d, READY, told to install and gone: its INSTALL echoed, then installing, then unheard. */
static void installs(uint8_t d)
{
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(d)->op);
    says(d, PLINK_FWS_INSTALLING, 0u);
    step();
    expect(d, PLINK_FWK_INSTALLING, 0u);
    kp(d)->online = false;
    kp(d)->away = true;
    NOW += 20000u;
    step();
}

/* The master confirmed on version (its next release; the image's bytes differ). */
static void master_on(const char *version)
{
    str(W.run.image.version, sizeof W.run.image.version, version);
    W.run.image.sha256[0] ^= 0x5Au;
    W.own = PLINK_FWM_IDLE;
}

/* The master restarts: the planner's memory goes, the holds stay (link/fwgo); every keypad comes
 * back in a new session. */
static void master_restart(void)
{
    memset(W.track, 0, sizeof W.track);
    W.start_ms = NOW;
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        plink_fw_kp_t *k = &W.kp[i];
        k->sessions++;
        k->order = 0u; /* nothing outstanding in its new core, nothing kept from before */
        k->order_op = 0u;
        k->order_acked = false;
        k->have_state = k->state_now = false;
    }
}

/* ---- the scripted keypad --------------------------------------------------------------------- */

static bool run_exists(uint8_t s)
{
    return s >= PLINK_FWS_PROBING && s <= PLINK_FWS_INSTALLING;
}

static unsigned model_fails(model_t *m)
{
    unsigned n = 0u;
    for (size_t k = 0; k < 3u; k++) {
        n += m->fails[k] != 0u && NOW < m->fails[k] + DAY_MS ? 1u : 0u;
    }
    return n;
}

/* Before a step: restarts and stalls fall due, and away is worked out as the core does. */
static void model_time(void)
{
    for (uint8_t d = 1u; d <= PLINK_KEYPADS_MAX; d++) {
        model_t *m = &M[d - 1u];
        plink_fw_kp_t *k = kp(d);
        if (m->mode == M_OFF) {
            continue;
        }
        if (m->restart_at != 0u && NOW >= m->restart_at) {
            m->restart_at = 0u;
            k->sessions++;
            k->online = true;
            k->info_now = true;
            k->state_now = false; /* no run: no FW_STATE in its new session */
            k->state.order = 0u;  /* its core restarted: no order, no run */
            k->state.op = 0u;
            k->state.state = PLINK_FWS_IDLE;
            k->state.why = PLINK_FW_WHY_OK;
            k->state.last = m->mode == M_GOOD ? (uint8_t)PLINK_FW_LAST_UPDATED
                                              : (uint8_t)PLINK_FW_LAST_ROLLED_BACK;
            str(k->state.last_version, sizeof k->state.last_version, m->target);
            if (m->mode == M_GOOD) {
                str(k->info.version, sizeof k->info.version, m->target);
                k->state_now = true;
                k->state.state = PLINK_FWS_IDLE;
                k->state.probation = 0u;
                k->state_ms = NOW;
            }
            if (m->mode == M_LATE) {
                str(k->info.version, sizeof k->info.version, m->target);
                memset(&k->state, 0, sizeof k->state); /* probation 0, last NONE */
                k->have_state = k->state_now = true;
                k->state_ms = NOW;
                m->back_at = NOW + 70000u;
            }
        }
        if (m->back_at != 0u && NOW >= m->back_at) {
            m->back_at = 0u;
            k->sessions++;
            str(k->info.version, sizeof k->info.version, m->prev);
            memset(&k->state, 0, sizeof k->state);
            k->have_state = k->state_now = true;
            k->state_ms = NOW;
        }
        if (k->online && k->state_now && run_exists(k->state.state)) {
            k->state_ms = NOW; /* a run's FW_STATE every 10 s */
        }
        if (m->stall_at != 0u && NOW >= m->stall_at) {
            m->stall_at = 0u;
            for (size_t f = 0; f < 3u; f++) {
                if (m->fails[f] == 0u || NOW >= m->fails[f] + DAY_MS) {
                    m->fails[f] = NOW;
                    break;
                }
            }
            says(d, PLINK_FWS_FAILED, PLINK_FW_WHY_STALLED);
        }
        k->away = !k->online ||
                  (k->order != 0u && !k->order_acked && NOW >= k->order_ms + 30000u) ||
                  (k->have_state && run_exists(k->state.state) && NOW >= k->state_ms + 30000u);
    }
}

/* After a step: each scripted keypad answers the order it was sent. */
static void model_orders(void)
{
    for (uint8_t d = 1u; d <= PLINK_KEYPADS_MAX; d++) {
        model_t *m = &M[d - 1u];
        const plink_fw_order_t *o = sent(d);
        if (m->mode == M_OFF || o->op == 0u) {
            continue;
        }
        m->ops[o->op & 7u]++;
        if (o->version[0] != '\0') {
            str(m->target, sizeof m->target, o->version);
        }
        switch (o->op) {
        case PLINK_FW_OP_PROBE: {
            plink_fw_state_t *s = says(d, PLINK_FWS_IDLE, PLINK_FW_WHY_OK);
            s->asked = 1u;
            s->probe_why = PLINK_FW_WHY_OK;
            str(s->offered, sizeof s->offered, o->version);
            break;
        }
        case PLINK_FW_OP_FETCH: ready(d); break;
        case PLINK_FW_OP_RELAY:
            if (m->mode != M_STALL) {
                ready(d);
            } else if (model_fails(m) >= 3u) {
                says(d, PLINK_FWS_IDLE, PLINK_FW_WHY_BUDGET);
            } else {
                says(d, PLINK_FWS_RECEIVING, PLINK_FW_WHY_OK);
                m->stall_at = NOW + 60000u;
            }
            break;
        case PLINK_FW_OP_INSTALL:
            ready(d); /* its gate passes; it restarts a moment later */
            str(m->prev, sizeof m->prev, kp(d)->info.version);
            m->restart_at = NOW + 2000u;
            break;
        case PLINK_FW_OP_CANCEL: says(d, PLINK_FWS_STOPPED, PLINK_FW_WHY_CANCELLED); break;
        default: says(d, kp(d)->state.state, PLINK_FW_WHY_OK); break;
        }
    }
}

/* ms of steps every `every`, the master's check answered each hour. */
static void run_model(uint64_t ms, uint64_t every)
{
    const uint64_t end = NOW + ms;
    while (NOW < end) {
        if (NOW >= W.check_ms + HOUR_MS) {
            W.check_ms = NOW;
        }
        model_time();
        step();
        model_orders();
        NOW += every;
    }
}

/* ---- the path (LINK-031) --------------------------------------------------------------------- */

enum {
    X_DEVICE = 0x01,   /* the keypad is a 10.1" */
    X_SERVER = 0x02,   /* the master doesn't reach its server */
    X_RELAY = 0x04,    /* the keypad takes no relay */
    X_CHIP = 0x08,     /* a v3 chip, outside the image's range */
    X_PROJECT = 0x10,  /* another project */
    X_DEV = 0x20,      /* a development build of the same project: it accepts the release */
    X_SLOT = 0x40,     /* a slot smaller than the image */
};

typedef struct {
    uint8_t why;
    const char *offered;
    uint8_t setup;
    uint8_t state, path, block;
    uint8_t op;      /* the order that goes at once */
    bool copy;       /* a copy is asked for with its facts */
} path_row_t;

static const path_row_t PATHS[] = {
    {PLINK_FW_WHY_OK, V, 0u, PLINK_FWK_FETCH, PLINK_FWK_PATH_FETCH, 0u, PLINK_FW_OP_FETCH, false},
    {PLINK_FW_WHY_OK, V, X_DEVICE, PLINK_FWK_FETCH, PLINK_FWK_PATH_FETCH, 0u, PLINK_FW_OP_FETCH,
     false},
    {PLINK_FW_WHY_OK, "1.4.1", 0u, PLINK_FWK_RELAY, PLINK_FWK_PATH_RELAY_OWN, 0u,
     PLINK_FW_OP_RELAY, false},
    {PLINK_FW_WHY_OK, "", 0u, PLINK_FWK_RELAY, PLINK_FWK_PATH_RELAY_OWN, 0u, PLINK_FW_OP_RELAY,
     false},
    {PLINK_FW_WHY_UP_TO_DATE, "", 0u, PLINK_FWK_RELAY, PLINK_FWK_PATH_RELAY_OWN, 0u,
     PLINK_FW_OP_RELAY, false},
    {PLINK_FW_WHY_OTHER_VERSION, "1.3.5", 0u, PLINK_FWK_RELAY, PLINK_FWK_PATH_RELAY_OWN, 0u,
     PLINK_FW_OP_RELAY, false},
    {PLINK_FW_WHY_OTHER_VERSION, "1.3.5", X_DEVICE, PLINK_FWK_RELAY_WAIT, PLINK_FWK_PATH_NONE, 0u,
     0u, true},
    {PLINK_FW_WHY_OTHER_VERSION, "1.3.5", X_DEVICE | X_SERVER, PLINK_FWK_BLOCKED,
     PLINK_FWK_PATH_NONE, PLINK_FWK_WHY_NO_IMAGE, 0u, false},
    /* UNKNOWN_VERSION: the master's own slot only, never a copy (P2 Q16). */
    {PLINK_FW_WHY_UNKNOWN_VERSION, "", 0u, PLINK_FWK_RELAY, PLINK_FWK_PATH_RELAY_OWN, 0u,
     PLINK_FW_OP_RELAY, false},
    {PLINK_FW_WHY_UNKNOWN_VERSION, "", X_SERVER, PLINK_FWK_RELAY, PLINK_FWK_PATH_RELAY_OWN, 0u,
     PLINK_FW_OP_RELAY, false},
    {PLINK_FW_WHY_UNKNOWN_VERSION, "", X_DEVICE, PLINK_FWK_BLOCKED, PLINK_FWK_PATH_NONE,
     PLINK_FW_WHY_UNKNOWN_VERSION, 0u, false},
    {PLINK_FW_WHY_UNKNOWN_VERSION, "", X_CHIP, PLINK_FWK_BLOCKED, PLINK_FWK_PATH_NONE,
     PLINK_FW_WHY_UNKNOWN_VERSION, 0u, false},
    {PLINK_FW_WHY_UNKNOWN_VERSION, "", X_RELAY, PLINK_FWK_BLOCKED, PLINK_FWK_PATH_NONE,
     PLINK_FWK_WHY_NO_IMAGE, 0u, false},
    /* The server refuses the keypad: never relayed past its rules. */
    {PLINK_FW_WHY_UNKNOWN_DEVICE, "", 0u, PLINK_FWK_BLOCKED, PLINK_FWK_PATH_NONE,
     PLINK_FW_WHY_UNKNOWN_DEVICE, 0u, false},
    {PLINK_FW_WHY_HELD_BACK, "", 0u, PLINK_FWK_BLOCKED, PLINK_FWK_PATH_NONE,
     PLINK_FW_WHY_HELD_BACK, 0u, false},
    {PLINK_FW_WHY_STEPPING_STONE, "", 0u, PLINK_FWK_BLOCKED, PLINK_FWK_PATH_NONE,
     PLINK_FW_WHY_STEPPING_STONE, 0u, false},
    {PLINK_FW_WHY_NOT_OFFERED, "", 0u, PLINK_FWK_BLOCKED, PLINK_FWK_PATH_NONE,
     PLINK_FW_WHY_NOT_OFFERED, 0u, false},
    {17u, "", 0u, PLINK_FWK_BLOCKED, PLINK_FWK_PATH_NONE, 17u, 0u, false}, /* unknown 10-19 */
    {19u, "", 0u, PLINK_FWK_BLOCKED, PLINK_FWK_PATH_NONE, 19u, 0u, false},
    /* Not reached. */
    {PLINK_FW_WHY_NO_NETWORK, "", 0u, PLINK_FWK_RELAY, PLINK_FWK_PATH_RELAY_OWN, 0u,
     PLINK_FW_OP_RELAY, false},
    {PLINK_FW_WHY_NO_LOOKUP, "", 0u, PLINK_FWK_RELAY, PLINK_FWK_PATH_RELAY_OWN, 0u,
     PLINK_FW_OP_RELAY, false},
    {PLINK_FW_WHY_SERVER_ERROR, "", 0u, PLINK_FWK_RELAY, PLINK_FWK_PATH_RELAY_OWN, 0u,
     PLINK_FW_OP_RELAY, false},
    {8u, "", 0u, PLINK_FWK_RELAY, PLINK_FWK_PATH_RELAY_OWN, 0u, PLINK_FW_OP_RELAY,
     false}, /* unknown 1-9: not reached */
    {PLINK_FW_WHY_NO_CONNECT, "", X_DEV, PLINK_FWK_RELAY, PLINK_FWK_PATH_RELAY_OWN, 0u,
     PLINK_FW_OP_RELAY, false},
    {PLINK_FW_WHY_NO_CONNECT, "", X_DEVICE, PLINK_FWK_RELAY_WAIT, PLINK_FWK_PATH_NONE, 0u, 0u,
     true},
    {PLINK_FW_WHY_NO_CONNECT, "", X_CHIP, PLINK_FWK_RELAY_WAIT, PLINK_FWK_PATH_NONE, 0u, 0u, true},
    {PLINK_FW_WHY_NO_CONNECT, "", X_PROJECT, PLINK_FWK_RELAY_WAIT, PLINK_FWK_PATH_NONE, 0u, 0u,
     true},
    {PLINK_FW_WHY_NO_CONNECT, "", X_SLOT, PLINK_FWK_RELAY_WAIT, PLINK_FWK_PATH_NONE, 0u, 0u, true},
    {PLINK_FW_WHY_NO_CONNECT, "", X_DEVICE | X_SERVER, PLINK_FWK_BLOCKED, PLINK_FWK_PATH_NONE,
     PLINK_FWK_WHY_NO_IMAGE, 0u, false},
    {PLINK_FW_WHY_NO_CONNECT, "", X_SERVER, PLINK_FWK_RELAY, PLINK_FWK_PATH_RELAY_OWN, 0u,
     PLINK_FW_OP_RELAY, false}, /* its own slot needs no server */
    {PLINK_FW_WHY_NO_CONNECT, "", X_RELAY, PLINK_FWK_BLOCKED, PLINK_FWK_PATH_NONE,
     PLINK_FWK_WHY_NO_IMAGE, 0u, false},
    /* A transfer reason reads as not reached. */
    {PLINK_FW_WHY_BROKE_OFF, "", 0u, PLINK_FWK_RELAY, PLINK_FWK_PATH_RELAY_OWN, 0u,
     PLINK_FW_OP_RELAY, false},
    /* A waiting reason is no answer: it never asked its server - never relayed (LINK-031), probed
     * again (test_a_waiting_probe_is_asked_again). */
    {PLINK_FW_WHY_BUSY, "", 0u, PLINK_FWK_FAILED, PLINK_FWK_PATH_NONE, PLINK_FW_WHY_BUSY, 0u,
     false},
    {PLINK_FW_WHY_NOT_QUIET, "", 0u, PLINK_FWK_FAILED, PLINK_FWK_PATH_NONE, PLINK_FW_WHY_NOT_QUIET,
     0u, false},
    {58u, "", 0u, PLINK_FWK_FAILED, PLINK_FWK_PATH_NONE, 58u, 0u, false}, /* unknown 50-59 */
    {PLINK_FW_WHY_KEY, "", 0u, PLINK_FWK_BLOCKED, PLINK_FWK_PATH_NONE, PLINK_FW_WHY_KEY, 0u, false},
    {PLINK_FW_WHY_CHIP, "", 0u, PLINK_FWK_BLOCKED, PLINK_FWK_PATH_NONE, PLINK_FW_WHY_CHIP, 0u,
     false},
    {PLINK_FW_WHY_STOPPED, "", 0u, PLINK_FWK_STOPPED, PLINK_FWK_PATH_NONE, PLINK_FW_WHY_STOPPED,
     0u, false},
    {75u, "", 0u, PLINK_FWK_BLOCKED, PLINK_FWK_PATH_NONE, 75u, 0u, false}, /* a class unknown */
};

/* LINK-031: the PROBE's answer picks the path, row by row of "Choosing the path, per keypad". */
static void test_the_path_table(void)
{
    char msg[48];
    for (size_t r = 0; r < sizeof PATHS / sizeof PATHS[0]; r++) {
        const path_row_t *row = &PATHS[r];
        setUp();
        plink_fw_kp_t *k = keypad(1u, OLD);
        if ((row->setup & X_DEVICE) != 0u) {
            str(k->info.device, sizeof k->info.device, DEV10);
        }
        if ((row->setup & X_SERVER) != 0u) {
            W.check_ms = 0u;
        }
        if ((row->setup & X_RELAY) != 0u) {
            k->info.can = PLINK_FWI_FETCH;
        }
        if ((row->setup & X_CHIP) != 0u) {
            v3(1u);
        }
        if ((row->setup & X_PROJECT) != 0u) {
            str(k->info.project, sizeof k->info.project, "roboalarms-cyd-release");
        }
        if ((row->setup & X_DEV) != 0u) {
            str(k->info.project, sizeof k->info.project, "roboalarms-p4-dev");
        }
        if ((row->setup & X_SLOT) != 0u) {
            k->info.slot = 3000000u;
        }
        probed(1u, row->why, row->offered);
        snprintf(msg, sizeof msg, "row %u", (unsigned)r);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(row->state, tr(1u)->state, msg);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(row->path, tr(1u)->path, msg);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(row->block, tr(1u)->why, msg);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(row->op, sent(1u)->op, msg);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(row->copy ? 1u : 0u, S.copy_for, msg);
        if (row->op == PLINK_FW_OP_RELAY) {
            /* The running image: its size, SHA-256 (id8) and key, with the relay's pace. */
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(PLINK_FW_KIND_APP, sent(1u)->kind, msg);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(V, sent(1u)->version, msg);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(W.run.image.size, sent(1u)->size, msg);
            TEST_ASSERT_EQUAL_MEMORY_MESSAGE(W.run.image.sha256, sent(1u)->sha256,
                                             PLINK_SHA256_LEN, msg);
            TEST_ASSERT_EQUAL_MEMORY_MESSAGE(W.run.image.key, sent(1u)->key, PLINK_FW_KEY_LEN,
                                             msg);
            TEST_ASSERT_EQUAL_UINT16_MESSAGE(PLINK_FWP_PAGE, sent(1u)->page, msg);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(PLINK_FWP_COUNT, sent(1u)->count, msg);
        }
        if (row->op == PLINK_FW_OP_FETCH) {
            TEST_ASSERT_EQUAL_STRING_MESSAGE(V, sent(1u)->version, msg);
            TEST_ASSERT_EQUAL_STRING_MESSAGE("roboalarms.com", sent(1u)->server, msg);
            TEST_ASSERT_EQUAL_STRING_MESSAGE("stable", sent(1u)->channel, msg);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, sent(1u)->size, msg);
        }
    }
}

/* LINK-031: UNKNOWN_VERSION waits for the own slot while it isn't worked out, and never asks for a
 * copy. */
static void test_unknown_version_waits_for_the_own_slot(void)
{
    keypad(1u, OLD);
    W.run.have = false;
    probed(1u, PLINK_FW_WHY_UNKNOWN_VERSION, "");
    expect(1u, PLINK_FWK_RELAY_WAIT, 0u);
    TEST_ASSERT_EQUAL_UINT8(0u, S.copy_for);
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    W.run.have = true;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWK_PATH_RELAY_OWN, tr(1u)->path);
}

/* LINK-031: PROBE comes first, at the master's server and channel for exactly the target; a keypad
 * that can't fetch goes to the relay without one; a PROBE echoed and then silent for 15 s (a
 * keypad probing says so every 10 s) lost its answer - asked again; a PROBE unanswered for 2 min
 * reads as not reached. */
static void test_probe_comes_first(void)
{
    keypad(1u, OLD);
    keypad(2u, OLD)->info.can = PLINK_FWI_RELAY;
    keypad(3u, OLD);
    const uint64_t t0 = NOW;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_KIND_APP, sent(1u)->kind);
    TEST_ASSERT_EQUAL_STRING(V, sent(1u)->version);
    TEST_ASSERT_EQUAL_STRING("roboalarms.com", sent(1u)->server);
    TEST_ASSERT_EQUAL_STRING("stable", sent(1u)->channel);
    expect(1u, PLINK_FWK_PROBING, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(2u)->op);
    expect(2u, PLINK_FWK_RELAY, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(3u)->op);
    /* The echo is no answer, nor is an answer made before the PROBE went. */
    says(1u, PLINK_FWS_PROBING, 0u);
    plink_fw_state_t *s = says(3u, PLINK_FWS_IDLE, 0u);
    s->asked = 1u;
    s->age_s = 5u;
    s->probe_why = PLINK_FW_WHY_OK;
    str(s->offered, sizeof s->offered, V);
    NOW += 1000u;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    expect(1u, PLINK_FWK_PROBING, 0u);
    expect(3u, PLINK_FWK_PROBING, 0u);
    /* Keypad 1 probes on (its FW_STATE every 10 s); keypad 3 says nothing more: its answer was
     * lost, and it is asked again 15 s after its echo. */
    NOW += 10000u;
    says(1u, PLINK_FWS_PROBING, 0u);
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    NOW += 4000u;
    step();
    TEST_ASSERT_EQUAL_UINT(1u, orders());
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(3u)->op);
    says(3u, PLINK_FWS_PROBING, 0u);
    NOW += 1000u;
    answer_probe(3u, PLINK_FW_WHY_OK, V);
    step();
    expect(3u, PLINK_FWK_FETCH, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_FETCH, sent(3u)->op);
    /* Keypad 1 is never asked again while it probes (the core resends); its FW_STATEs saying it
     * still probes keep the relay off past 2 min - an answer is still to come, and the relay
     * never goes past it - up to 10 min, when it reads as not reached all the same. */
    while (NOW + 10000u < t0 + PLINK_FWP_PROBE_MAX_MS) {
        NOW += 10000u;
        says(1u, PLINK_FWS_PROBING, 0u);
        step();
        TEST_ASSERT_EQUAL_UINT8(0u, sent(1u)->op);
        expect(1u, PLINK_FWK_PROBING, 0u);
    }
    NOW = t0 + PLINK_FWP_PROBE_MAX_MS;
    step();
    expect(1u, PLINK_FWK_RELAY_WAIT, 0u); /* the relay's one turn is keypad 2's */
    TEST_ASSERT_EQUAL_UINT8(0u, sent(1u)->op);
    /* One that falls silent off line after its echo reads as not reached 2 min after its PROBE. */
    setUp();
    keypad(1u, OLD);
    const uint64_t t1 = NOW;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
    says(1u, PLINK_FWS_PROBING, 0u);
    kp(1u)->online = false;
    NOW = t1 + 2u * MIN_MS - 1u;
    step();
    expect(1u, PLINK_FWK_PROBING, 0u);
    NOW += 1u;
    step();
    expect(1u, PLINK_FWK_RELAY_WAIT, 0u);
}

/* LINK-031: a probe's answer that comes after its relay went (a check slower than its wait) still
 * counts: a server's refusal ends the relay - blocked, its run cancelled, never relayed past the
 * server's rules; another version offered lets it go on; an answer from before its PROBE never
 * counts. */
static void test_a_late_refusal_ends_the_relay(void)
{
    keypad(1u, OLD);
    keypad(2u, OLD);
    const uint64_t t0 = NOW;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(2u)->op);
    says(1u, PLINK_FWS_PROBING, 0u);
    says(2u, PLINK_FWS_PROBING, 0u);
    kp(1u)->online = kp(2u)->online = false;
    NOW = t0 + 2u * MIN_MS;
    step();
    expect(1u, PLINK_FWK_RELAY_WAIT, 0u);
    expect(2u, PLINK_FWK_RELAY_WAIT, 0u);
    kp(1u)->online = kp(2u)->online = true;
    NOW += 1000u;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    says(1u, PLINK_FWS_RECEIVING, 0u);
    NOW += 20000u;
    step();
    expect(1u, PLINK_FWK_RELAY, 0u);
    /* Keypad 2's answer made before its PROBE: nothing; keypad 1's, made 10 s ago: HELD_BACK. */
    plink_fw_state_t *s = says(2u, PLINK_FWS_IDLE, 0u);
    s->asked = 1u;
    s->age_s = (uint32_t)((NOW - t0) / 1000u + 5u);
    s->probe_why = PLINK_FW_WHY_HELD_BACK;
    s = says(1u, PLINK_FWS_RECEIVING, 0u);
    s->asked = 1u;
    s->age_s = 10u;
    s->probe_why = PLINK_FW_WHY_HELD_BACK;
    step();
    expect(1u, PLINK_FWK_BLOCKED, PLINK_FW_WHY_HELD_BACK);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sent(1u)->op);
    expect(2u, PLINK_FWK_RELAY, 0u); /* the relay's turn is free: keypad 2's */
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(2u)->op);
    /* Another version offered late: its relay goes on. */
    says(2u, PLINK_FWS_RECEIVING, 0u);
    NOW += 1000u;
    s = says(2u, PLINK_FWS_RECEIVING, 0u);
    s->asked = 1u;
    s->probe_why = PLINK_FW_WHY_OTHER_VERSION;
    str(s->offered, sizeof s->offered, "1.3.5");
    step();
    expect(2u, PLINK_FWK_RELAY, 0u);
    TEST_ASSERT_EQUAL_UINT(0u, orders());
}

/* LINK-030: orders go only while the master runs no update of its own, isn't on its self-test and
 * is quiet, to a keypad in a session with FW on frames of 250 B, heard within 30 s. */
static void test_orders_only_while_allowed(void)
{
    keypad(1u, OLD);
    const uint8_t own[] = {PLINK_FWM_UPDATING, PLINK_FWM_SELF_TEST};
    for (size_t i = 0; i < sizeof own; i++) {
        W.own = own[i];
        step();
        TEST_ASSERT_EQUAL_UINT(0u, orders());
        expect(1u, PLINK_FWK_BEHIND, 0u);
    }
    W.own = PLINK_FWM_IDLE;
    W.quiet = false;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    W.quiet = true;
    kp(1u)->fw = false; /* frames under 250 B, or no FW in its session */
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    kp(1u)->fw = true;
    kp(1u)->online = false;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    kp(1u)->online = true;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
}

/* OTA-014: a keypad on the target or newer is left alone; a version that doesn't parse is never
 * replaced either. */
static void test_never_older(void)
{
    keypad(1u, V);
    keypad(2u, "1.5.0");
    keypad(3u, "abc1234");
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    expect(1u, PLINK_FWK_CURRENT, 0u);
    expect(2u, PLINK_FWK_CURRENT, 0u);
    expect(3u, PLINK_FWK_CURRENT, 0u);
    TEST_ASSERT_EQUAL_HEX8(0u, S.running);
    TEST_ASSERT_EQUAL_HEX8(0u, S.held);
}

/* Decided 2026-10-17 (LINK-030): keypads never fall behind, with no person's step. The master's own
 * update, once confirmed, sends every keypad behind on its way (nothing while it is on its
 * self-test); a keypad found behind later is brought up by itself: newly paired (its FW_INFO
 * asked for first), back on line, or its FW_INFO naming an older version. */
static void test_keypads_follow_the_master_by_themselves(void)
{
    keypad(1u, V);
    keypad(2u, V);
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    /* The master restarts into NEXT: on its self-test nothing goes. */
    str(W.run.image.version, sizeof W.run.image.version, NEXT);
    W.own = PLINK_FWM_SELF_TEST;
    W.start_ms = NOW;
    NOW += 2u * MIN_MS;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    expect(1u, PLINK_FWK_BEHIND, 0u);
    expect(2u, PLINK_FWK_BEHIND, 0u);
    /* Confirmed: both are told at once. */
    W.own = PLINK_FWM_IDLE;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
    TEST_ASSERT_EQUAL_STRING(NEXT, sent(1u)->version);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(2u)->op);
    TEST_ASSERT_EQUAL_HEX8(0x03u, S.running);
    /* Found behind later: newly paired (no FW_INFO yet: INFO first). */
    setUp();
    keypad(1u, V);
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    keypad(2u, OLD);
    kp(2u)->have_info = kp(2u)->info_now = false;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(2u)->op);
    expect(2u, PLINK_FWK_UNKNOWN, 0u);
    kp(2u)->have_info = kp(2u)->info_now = true;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(2u)->op);
    /* Back on line. */
    keypad(3u, OLD)->online = false;
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, sent(3u)->op);
    expect(3u, PLINK_FWK_BEHIND, 0u);
    kp(3u)->online = true;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(3u)->op);
    /* Its FW_INFO older (put back with a cable). */
    str(kp(1u)->info.version, sizeof kp(1u)->info.version, OLD);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
}

/* LINK-030: PROBE and FETCH go to every keypad at once. */
static void test_probe_and_fetch_to_all_at_once(void)
{
    for (uint8_t d = 1u; d <= 4u; d++) {
        keypad(d, OLD);
    }
    step();
    TEST_ASSERT_EQUAL_UINT(4u, orders());
    for (uint8_t d = 1u; d <= 4u; d++) {
        TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(d)->op);
        says(d, PLINK_FWS_PROBING, 0u);
    }
    NOW += 2000u;
    for (uint8_t d = 1u; d <= 4u; d++) {
        answer_probe(d, PLINK_FW_WHY_OK, V);
    }
    step();
    TEST_ASSERT_EQUAL_UINT(4u, orders());
    for (uint8_t d = 1u; d <= 4u; d++) {
        TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_FETCH, sent(d)->op);
        expect(d, PLINK_FWK_FETCH, 0u);
    }
    /* Fetching: nothing more until the runs move. */
    for (uint8_t d = 1u; d <= 4u; d++) {
        says(d, PLINK_FWS_FETCHING, 0u);
    }
    NOW += 1000u;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
}

/* LINK-030: one relay receives at a time, the waiting keypad with the lowest number next; a keypad
 * waiting for its new key holds no turn, and once its key is accepted while another relays it
 * waits for its turn again - its run cancelled, so two relays are never served at once - and
 * relays at its turn. */
static void test_one_relay_at_a_time(void)
{
    for (uint8_t d = 1u; d <= 3u; d++) {
        keypad(d, OLD)->info.can = PLINK_FWI_RELAY;
    }
    step();
    TEST_ASSERT_EQUAL_UINT(1u, orders());
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    expect(1u, PLINK_FWK_RELAY, 0u);
    expect(2u, PLINK_FWK_RELAY_WAIT, 0u);
    expect(3u, PLINK_FWK_RELAY_WAIT, 0u);
    says(1u, PLINK_FWS_RECEIVING, 0u);
    NOW += 30000u;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    says(1u, PLINK_FWS_KEY_WAIT, 0u);
    step();
    expect(1u, PLINK_FWK_KEY_WAIT, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(2u)->op); /* the key waits for a person */
    TEST_ASSERT_EQUAL_UINT8(0u, sent(3u)->op);
    says(2u, PLINK_FWS_RECEIVING, 0u);
    says(1u, PLINK_FWS_RECEIVING, 0u); /* its key accepted: its run would go on */
    step();
    expect(1u, PLINK_FWK_RELAY_WAIT, 0u);
    expect(2u, PLINK_FWK_RELAY, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT(1u, orders());
    says(1u, PLINK_FWS_STOPPED, PLINK_FW_WHY_CANCELLED);
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    expect(1u, PLINK_FWK_RELAY_WAIT, 0u);
    ready(2u);
    step();
    /* Keypad 1 - the lowest waiting - relays now, and keypad 2 installs. */
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(2u)->op);
    TEST_ASSERT_EQUAL_STRING(V, sent(2u)->version);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(3u)->op);
    says(1u, PLINK_FWS_RECEIVING, 0u);
    NOW += 1000u;
    ready(1u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(3u)->op);
}

/* LINK-030, LINK-034: one install at a time, the READY keypad with the lowest number first, once
 * none holds the turn; INSTALL names exactly the target; installing, then back on it in a new
 * session, the self-test (CHECKING), then UPDATED; the next goes only then. */
static void test_one_install_at_a_time(void)
{
    for (uint8_t d = 1u; d <= 3u; d++) {
        keypad(d, OLD);
    }
    all_fetching(3u);
    ready(3u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(3u)->op);
    TEST_ASSERT_EQUAL_STRING(V, sent(3u)->version);
    expect(3u, PLINK_FWK_READY, 0u);
    says(3u, PLINK_FWS_READY, 0u); /* the echo: its gate holds it a moment */
    ready(1u);
    ready(2u);
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    says(3u, PLINK_FWS_INSTALLING, 0u);
    step();
    expect(3u, PLINK_FWK_INSTALLING, 0u);
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    kp(3u)->online = false;
    NOW += 20000u;
    step();
    expect(3u, PLINK_FWK_INSTALLING, 0u);
    /* Back on V in a new session: its FW_INFO first, then its FW_STATE (probation). */
    back(3u, V, false, 0u, 0u);
    step();
    expect(3u, PLINK_FWK_CHECKING, 0u);
    back(3u, V, true, 1u, PLINK_FW_LAST_NONE);
    kp(3u)->sessions--; /* the same session */
    NOW += 30000u;
    step();
    expect(3u, PLINK_FWK_CHECKING, 0u);
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    kp(3u)->state.probation = 0u;
    kp(3u)->state.last = PLINK_FW_LAST_UPDATED;
    step();
    expect(3u, PLINK_FWK_UPDATED, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op); /* the lowest, not keypad 2 */
    TEST_ASSERT_EQUAL_UINT8(0u, sent(2u)->op);
}

/* LINK-030, LINK-034: an INSTALL its keypad's gate holds: back to READY, still holding the turn;
 * still READY in a new session (a rekey: its INSTALL hold went with that session) it is sent
 * INSTALL again and still holds the turn. An INSTALL never answered, its keypad away, is never
 * handed on while it may still be taken: the turn stays with that keypad (no other is told to
 * install) while the core resends it, and heard again it takes it. A day without an answer: a
 * CANCEL replaces it at once - the keypad still away - and only then does the turn go on. A
 * keypad the core says is installing or on its self-test (one the planner doesn't follow) holds
 * the turn too. */
static void test_install_waits_at_the_gate_and_away(void)
{
    keypad(1u, OLD);
    keypad(2u, OLD);
    all_fetching(2u);
    ready(1u);
    ready(2u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op);
    says(1u, PLINK_FWS_READY, 0u);
    says(1u, PLINK_FWS_INSTALLING, 0u);
    step();
    expect(1u, PLINK_FWK_INSTALLING, 0u);
    says(1u, PLINK_FWS_READY, 0u); /* the second ask said no */
    step();
    expect(1u, PLINK_FWK_READY, 0u);
    TEST_ASSERT_EQUAL_UINT(0u, orders()); /* keypad 2 still waits */
    back(1u, OLD, true, 0u, 0u);          /* a rekey: a new session, no restart */
    says(1u, PLINK_FWS_READY, 0u);
    step();
    expect(1u, PLINK_FWK_READY, 0u);
    TEST_ASSERT_TRUE(tr(1u)->installed);
    TEST_ASSERT_EQUAL_UINT(1u, orders());
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op); /* again: its hold went */
    TEST_ASSERT_EQUAL_STRING(V, sent(1u)->version);
    says(1u, PLINK_FWS_READY, 0u);
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    /* A new INSTALL never answered, its keypad away (it may have taken it, its echo lost): the
     * turn stays with it for 23 h - keypad 2 is never told - and heard again, it takes it. */
    setUp();
    keypad(1u, OLD);
    keypad(2u, OLD);
    all_fetching(2u);
    ready(1u);
    ready(2u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op);
    kp(1u)->online = false;
    kp(1u)->away = true;
    for (unsigned h = 0; h < 23u; h++) {
        NOW += HOUR_MS;
        kp(2u)->state_ms = NOW; /* its FW_STATE, every 10 s while READY */
        step();
        TEST_ASSERT_EQUAL_UINT(0u, orders());
        expect(1u, PLINK_FWK_READY, 0u);
        TEST_ASSERT_TRUE(tr(1u)->installed);
    }
    kp(1u)->online = true;
    kp(1u)->away = false;
    kp(2u)->state_ms = NOW;
    says(1u, PLINK_FWS_READY, 0u); /* the INSTALL, echoed at last */
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    says(1u, PLINK_FWS_INSTALLING, 0u);
    step();
    expect(1u, PLINK_FWK_INSTALLING, 0u);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(2u)->op);
    /* A day without an answer: a CANCEL replaces it at once, the keypad still away, and the turn
     * goes to keypad 2. */
    setUp();
    keypad(1u, OLD);
    keypad(2u, OLD);
    all_fetching(2u);
    ready(1u);
    ready(2u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op);
    kp(1u)->online = false;
    kp(1u)->away = true;
    NOW += PLINK_FWP_HOLD_MS - 1u;
    kp(2u)->state_ms = NOW;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    NOW += 1u;
    kp(2u)->state_ms = NOW;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sent(1u)->op);
    expect(1u, PLINK_FWK_BEHIND, 0u);
    TEST_ASSERT_FALSE(tr(1u)->installed);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(2u)->op);
    NOW += 1000u;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders()); /* once: the core resends it */
    /* The core says keypad 1 is on its self-test (an install of its own): keypad 2 waits. */
    setUp();
    keypad(1u, V)->busy = true;
    keypad(2u, OLD);
    probed(2u, PLINK_FW_WHY_OK, V);
    ready(2u);
    step();
    expect(2u, PLINK_FWK_READY, 0u);
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    kp(1u)->busy = false;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(2u)->op);
}

/* LINK-030, the canary: a rollback holds its device and chip revision on their version (one READY
 * is cancelled); another chip revision goes on. The hold is kept in link/fwgo and outlasts days
 * of the master's checks - it ends only at the master's next version, which is tried once. */
static void test_a_rollback_holds_its_model_until_the_next_version(void)
{
    for (uint8_t d = 1u; d <= 3u; d++) {
        keypad(d, OLD);
    }
    v3(3u);
    all_fetching(3u);
    for (uint8_t d = 1u; d <= 3u; d++) {
        ready(d);
    }
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, sent(3u)->op);
    installs(1u);
    NOW += 70000u;
    kp(2u)->state_ms = kp(3u)->state_ms = NOW; /* their FW_STATEs, every 10 s while READY */
    step();
    /* Its self-test failed: back on the old version, saying so. */
    back(1u, OLD, true, 0u, PLINK_FW_LAST_ROLLED_BACK);
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_ROLLED_BACK);
    expect(2u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sent(2u)->op);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(3u)->op); /* v3: another model */
    TEST_ASSERT_TRUE(S.holds_changed);
    TEST_ASSERT_EQUAL_STRING(V, S.holds.version);
    TEST_ASSERT_EQUAL_STRING(DEVICE, S.holds.canary[0].device);
    TEST_ASSERT_EQUAL_STRING("v1", S.holds.canary[0].chip_rev);
    TEST_ASSERT_EQUAL_HEX8(0x03u, S.held & 0x03u);
    says(2u, PLINK_FWS_STOPPED, PLINK_FW_WHY_CANCELLED);
    says(3u, PLINK_FWS_INSTALLING, 0u);
    step();
    TEST_ASSERT_FALSE(S.holds_changed);
    back(3u, V, true, 0u, PLINK_FW_LAST_UPDATED);
    step();
    expect(3u, PLINK_FWK_UPDATED, 0u);
    /* Three days of the master's checks, its keypads' facts unchanged: nothing more. */
    for (unsigned h = 0; h < 72u; h++) {
        NOW += HOUR_MS;
        W.check_ms = NOW;
        step();
        TEST_ASSERT_EQUAL_UINT(0u, orders());
    }
    expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_ROLLED_BACK);
    expect(2u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
    /* The master's next version: the hold ends, both are tried again. */
    master_on(NEXT);
    step();
    TEST_ASSERT_TRUE(S.holds_changed);
    TEST_ASSERT_EQUAL_STRING("", S.holds.version);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(2u)->op);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(3u)->op);
    TEST_ASSERT_EQUAL_STRING(NEXT, sent(1u)->version);
}

/* LINK-030: no return on the target within 10 min of installing holds the model as a rollback
 * does; so does one back on the target whose self-test's end is never heard (UPDATED only on the
 * keypad's word: its last section saying the target passed); one that turns up on the target
 * later, its self-test passed, ends its model's hold - and the next keypad of it, still READY,
 * installs what it holds. Probation over with nothing more said (its core before its service told
 * it) is no such word. */
static void test_no_return_within_10_min(void)
{
    keypad(1u, OLD);
    keypad(2u, OLD);
    all_fetching(2u);
    ready(1u);
    ready(2u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op);
    says(1u, PLINK_FWS_INSTALLING, 0u);
    step();
    kp(1u)->online = false;
    NOW += 10u * MIN_MS - 1u;
    step();
    expect(1u, PLINK_FWK_INSTALLING, 0u);
    NOW += 1u;
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_NO_RETURN);
    expect(2u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
    TEST_ASSERT_EQUAL_STRING(DEVICE, S.holds.canary[0].device);
    /* It turns up an hour later on V, its self-test running, then over: the hold ends. */
    NOW += HOUR_MS;
    back(1u, V, true, 1u, PLINK_FW_LAST_NONE);
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_NO_RETURN);
    kp(1u)->state.probation = 0u;
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_NO_RETURN);
    TEST_ASSERT_EQUAL_STRING(DEVICE, S.holds.canary[0].device);
    kp(1u)->state.last = PLINK_FW_LAST_UPDATED;
    step();
    expect(1u, PLINK_FWK_UPDATED, 0u);
    TEST_ASSERT_EQUAL_STRING("", S.holds.canary[0].device);
    expect(2u, PLINK_FWK_READY, 0u); /* its READY of before, taken up */
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(2u)->op);
    /* Again, but it comes back on V and says nothing more: NO_RETURN at 10 min; its word that the
     * self-test passed, heard then, ends the hold. */
    setUp();
    keypad(1u, OLD)->info.can = PLINK_FWI_FETCH;
    probed(1u, PLINK_FW_WHY_OK, V);
    ready(1u);
    step();
    says(1u, PLINK_FWS_INSTALLING, 0u);
    step();
    back(1u, V, false, 0u, 0u);
    NOW += 60000u;
    step();
    expect(1u, PLINK_FWK_CHECKING, 0u);
    NOW += 9u * MIN_MS;
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_NO_RETURN);
    TEST_ASSERT_TRUE(S.holds_changed);
    back(1u, V, true, 0u, PLINK_FW_LAST_UPDATED);
    kp(1u)->sessions--; /* the same session */
    step();
    expect(1u, PLINK_FWK_UPDATED, 0u);
    TEST_ASSERT_EQUAL_STRING("", S.holds.canary[0].device);
}

/* LINK-030, LINK-035: an INSTALL echoed (READY), then its keypad unheard for good: taken for
 * installing - never put back to READY by that echo of before - and NO_RETURN 10 min on, which
 * frees the install turn (keypad 2, another model, installs). Heard again still READY (its router
 * was down while it waited at its gate), it never restarted: its model's hold is undone and it
 * installs at its turn. */
static void test_an_install_unheard_is_no_return_at_10_min(void)
{
    keypad(1u, OLD);
    keypad(2u, OLD);
    v3(2u);
    W.run.chip_max = 399u; /* (the image runs on both) */
    all_fetching(2u);
    ready(1u);
    ready(2u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op);
    says(1u, PLINK_FWS_READY, 0u); /* echoed: its gate holds it a moment */
    step();
    kp(1u)->online = false;
    kp(1u)->away = true;
    NOW += 31000u;
    step();
    expect(1u, PLINK_FWK_INSTALLING, 0u);
    const uint64_t from = NOW;
    for (unsigned n = 0; n < 20u; n++) {
        NOW += 29000u;
        kp(2u)->state_ms = NOW; /* its FW_STATE, every 10 s while READY */
        step();
        expect(1u, PLINK_FWK_INSTALLING, 0u); /* not READY again on the echo of before */
        TEST_ASSERT_EQUAL_UINT(0u, orders());
    }
    NOW = from + PLINK_FWP_RETURN_MS;
    kp(2u)->state_ms = NOW;
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_NO_RETURN);
    TEST_ASSERT_EQUAL_STRING(DEVICE, S.holds.canary[0].device);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(2u)->op); /* the turn is free */
    /* ... and keypad 1, which may still hold its INSTALL (waiting at its gate, its link down), is
     * named the master's version at once, unheard: any order taken after an INSTALL voids its
     * hold (LINK-034), so it never restarts beside keypad 2 - the core resends it until echoed. */
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(1u)->op);
    TEST_ASSERT_EQUAL_STRING(V, sent(1u)->version);
    TEST_ASSERT_EQUAL_UINT(2u, orders());
    says(2u, PLINK_FWS_INSTALLING, 0u);
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    /* Back a minute later in the same session, still READY: it never restarted. */
    NOW += MIN_MS;
    kp(1u)->online = true;
    kp(1u)->away = false;
    kp(1u)->state_ms = NOW; /* a FW_STATE of its run, the INFO not yet echoed */
    kp(2u)->state_ms = NOW;
    step();
    expect(1u, PLINK_FWK_READY, 0u);
    TEST_ASSERT_FALSE(tr(1u)->installed);
    TEST_ASSERT_EQUAL_STRING("", S.holds.canary[0].device);
    TEST_ASSERT_TRUE(S.holds_changed);
    TEST_ASSERT_EQUAL_UINT(0u, orders()); /* keypad 2 holds the turn */
    back(2u, V, true, 0u, PLINK_FW_LAST_UPDATED);
    step();
    expect(2u, PLINK_FWK_UPDATED, 0u);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(1u)->op); /* its turn only once that INFO is echoed */
    says(1u, PLINK_FWS_READY, 0u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op);

    /* Its model's Try again now: the keypad that went back is sent on, echoes its INSTALL and is
     * unheard - NO_RETURN holds the model whole again; heard READY again, its Try again now is
     * its own again and it installs. */
    setUp();
    keypad(1u, OLD);
    probed(1u, PLINK_FW_WHY_OK, V);
    ready(1u);
    step();
    installs(1u);
    back(1u, OLD, true, 0u, PLINK_FW_LAST_ROLLED_BACK);
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_ROLLED_BACK);
    NOW += 1000u;
    W.again_ms[0] = NOW;
    step();
    TEST_ASSERT_EQUAL_UINT8(1u, S.holds.canary[0].retry);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
    says(1u, PLINK_FWS_PROBING, 0u);
    NOW += 1500u;
    answer_probe(1u, PLINK_FW_WHY_OK, V);
    step();
    ready(1u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op);
    says(1u, PLINK_FWS_READY, 0u);
    step();
    kp(1u)->online = false;
    kp(1u)->away = true;
    NOW += 31000u;
    step();
    NOW += PLINK_FWP_RETURN_MS;
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_NO_RETURN);
    TEST_ASSERT_EQUAL_STRING(DEVICE, S.holds.canary[0].device);
    TEST_ASSERT_EQUAL_UINT8(0u, S.holds.canary[0].retry);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(1u)->op); /* its INSTALL voided */
    NOW += MIN_MS;
    kp(1u)->online = true;
    kp(1u)->away = false;
    says(1u, PLINK_FWS_READY, 0u); /* the INFO echoed */
    step();
    TEST_ASSERT_EQUAL_STRING(DEVICE, S.holds.canary[0].device); /* the model's own, from before */
    TEST_ASSERT_EQUAL_UINT8(1u, S.holds.canary[0].retry);
    expect(1u, PLINK_FWK_READY, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op);
}

/* LINK-030: a keypad told to install went back only on its word. In a new session, its FW_STATE
 * there saying its READY image was discarded (24 h at its gate, its INSTALL hold gone with the
 * session it came in): fetched again, nothing counted; stopped by a person there: its stop.
 * Neither holds its model. */
static void test_a_keypad_that_never_restarted_holds_nothing(void)
{
    for (unsigned variant = 0; variant < 2u; variant++) {
        setUp();
        keypad(1u, OLD);
        keypad(2u, OLD);
        all_fetching(2u);
        ready(1u);
        ready(2u);
        step();
        TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op);
        says(1u, PLINK_FWS_READY, 0u);
        step();
        NOW += DAY_MS;
        kp(2u)->state_ms = NOW;
        kp(1u)->sessions++; /* a rekey: its FW_INFO there, then its FW_STATE */
        if (variant == 0u) {
            says(1u, PLINK_FWS_FAILED, PLINK_FW_WHY_TIMEOUT);
        } else {
            says(1u, PLINK_FWS_STOPPED, PLINK_FW_WHY_STOPPED);
        }
        step();
        TEST_ASSERT_EQUAL_STRING("", S.holds.canary[0].device);
        TEST_ASSERT_FALSE(tr(1u)->installed);
        for (size_t k = 0; k < PLINK_FWP_BUDGET; k++) {
            TEST_ASSERT_EQUAL_UINT64(0u, tr(1u)->fails[k]);
        }
        if (variant == 0u) {
            expect(1u, PLINK_FWK_PROBING, 0u); /* fetched again */
            TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
        } else {
            expect(1u, PLINK_FWK_STOPPED, PLINK_FW_WHY_STOPPED);
            TEST_ASSERT_NOT_EQUAL(0u, S.holds.stop_until_ms[0]);
        }
        TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(2u)->op); /* the turn goes on */
    }
}

/* LINK-030, LINK-035: a keypad told to install, back after 40 s away in a new session with its
 * FW_STATE there not yet in - the core counts it away from the FW_STATE of its earlier session - is
 * asked (INFO), not judged; still READY in its answer, it is told to install again. Nothing holds
 * its model. */
static void test_a_keypad_back_from_away_is_asked_not_judged(void)
{
    keypad(1u, OLD);
    keypad(2u, OLD);
    all_fetching(2u);
    ready(1u);
    ready(2u);
    step();
    says(1u, PLINK_FWS_READY, 0u);
    step();
    kp(1u)->online = false;
    kp(1u)->away = true;
    NOW += 40000u;
    kp(2u)->state_ms = NOW;
    step();
    kp(1u)->sessions++;
    kp(1u)->online = true;
    kp(1u)->info_now = true;
    kp(1u)->state_now = false;
    kp(1u)->away = true; /* (from its FW_STATE of the earlier session, 40 s old) */
    step();
    TEST_ASSERT_EQUAL_STRING("", S.holds.canary[0].device);
    TEST_ASSERT_TRUE(tr(1u)->installed);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(2u)->op);
    NOW += 2000u;
    says(1u, PLINK_FWS_READY, 0u); /* the answer: READY, it never restarted */
    kp(1u)->away = false;
    step();
    expect(1u, PLINK_FWK_READY, 0u);
    TEST_ASSERT_TRUE(tr(1u)->installed);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op);
    TEST_ASSERT_EQUAL_STRING("", S.holds.canary[0].device);
}

/* LINK-030: a keypad never seen installing (its INSTALL echoed READY, its INSTALLING lost or never
 * sent) that comes back on its old version with its core restarted (a power cut at its gate) never
 * tried the target unless its last section says so: given and naming no rollback of the target -
 * probed again at once; not given - 10 min on. No canary, nothing counted, the turn goes on. One
 * seen installing waits for its word: NO_RETURN 10 min on. */
static void test_a_restart_without_installing_holds_nothing(void)
{
    for (unsigned variant = 0; variant < 3u; variant++) {
        setUp();
        keypad(1u, OLD);
        keypad(2u, OLD);
        v3(2u);
        W.run.chip_max = 399u;
        all_fetching(2u);
        ready(1u);
        ready(2u);
        step();
        TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op);
        says(1u, PLINK_FWS_READY, 0u);
        step();
        if (variant == 2u) {
            says(1u, PLINK_FWS_INSTALLING, 0u);
            step();
            expect(1u, PLINK_FWK_INSTALLING, 0u);
        }
        NOW += 5000u;
        kp(2u)->state_ms = NOW;
        back(1u, OLD, true, 0u, variant == 0u ? PLINK_FW_LAST_UPDATED : PLINK_FW_LAST_NONE);
        str(kp(1u)->state.last_version, sizeof kp(1u)->state.last_version, OLD);
        step();
        TEST_ASSERT_EQUAL_STRING("", S.holds.canary[0].device);
        if (variant == 0u) {
            expect(1u, PLINK_FWK_PROBING, 0u);
            TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
            TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(2u)->op);
            for (size_t k = 0; k < PLINK_FWP_BUDGET; k++) {
                TEST_ASSERT_EQUAL_UINT64(0u, tr(1u)->fails[k]);
            }
            continue;
        }
        expect(1u, PLINK_FWK_INSTALLING, 0u); /* holding the turn while its word may come */
        TEST_ASSERT_EQUAL_UINT8(0u, sent(2u)->op);
        const uint64_t from = tr(1u)->since_ms;
        NOW = from + PLINK_FWP_RETURN_MS - 1u;
        kp(2u)->state_ms = NOW;
        step();
        expect(1u, PLINK_FWK_INSTALLING, 0u);
        NOW += 1u;
        step();
        if (variant == 1u) {
            expect(1u, PLINK_FWK_PROBING, 0u);
            TEST_ASSERT_EQUAL_STRING("", S.holds.canary[0].device);
        } else {
            expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_NO_RETURN);
            TEST_ASSERT_EQUAL_STRING(DEVICE, S.holds.canary[0].device);
        }
        TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(2u)->op);
    }
}

/* LINK-031, decision 4 (keypad updates): a PROBE answered with a waiting reason (busy, not quiet:
 * it never asked its server) is no answer - nothing is relayed past a server not asked. Probed
 * again 2 min on, nothing counted; Try again now ends the wait. */
static void test_a_waiting_probe_is_asked_again(void)
{
    keypad(1u, OLD);
    probed(1u, PLINK_FW_WHY_BUSY, "");
    expect(1u, PLINK_FWK_FAILED, PLINK_FW_WHY_BUSY);
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    TEST_ASSERT_EQUAL_HEX8(0x01u, S.held);
    for (size_t k = 0; k < PLINK_FWP_BUDGET; k++) {
        TEST_ASSERT_EQUAL_UINT64(0u, tr(1u)->fails[k]);
    }
    NOW += PLINK_FWP_RETRY_MS - 1u;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    NOW += 1u;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
    says(1u, PLINK_FWS_PROBING, 0u);
    NOW += 1500u;
    answer_probe(1u, PLINK_FW_WHY_OK, V);
    step();
    expect(1u, PLINK_FWK_FETCH, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_FETCH, sent(1u)->op);
}

/* LINK-032: a relay at KEY_WAIT when the master restarts - its new core holds no RELAY for it -
 * is taken up; once its key is accepted (RECEIVING) its RELAY goes again at once (the same image:
 * it continues by id8), so the master serves its FW_REQs and it never stalls. */
static void test_a_relay_at_key_wait_goes_on_after_a_master_restart(void)
{
    keypad(1u, OLD)->info.can = PLINK_FWI_RELAY;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    plink_fw_state_t *s = says(1u, PLINK_FWS_KEY_WAIT, 0u);
    s->size = W.run.image.size;
    memcpy(s->id8, W.run.image.sha256, PLINK_ID8_LEN);
    step();
    expect(1u, PLINK_FWK_KEY_WAIT, 0u);
    master_restart();
    NOW += 2000u;
    s = says(1u, PLINK_FWS_KEY_WAIT, 0u); /* at the first STATUS of its new session */
    s->order = 0x1234u;
    s->op = PLINK_FW_OP_RELAY;
    s->size = W.run.image.size;
    memcpy(s->id8, W.run.image.sha256, PLINK_ID8_LEN);
    step();
    expect(1u, PLINK_FWK_KEY_WAIT, 0u);
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    NOW += 30000u;
    s = says(1u, PLINK_FWS_RECEIVING, 0u); /* its key accepted at the keypad */
    s->order = 0x1234u;
    s->op = PLINK_FW_OP_RELAY;
    s->size = W.run.image.size;
    memcpy(s->id8, W.run.image.sha256, PLINK_ID8_LEN);
    step();
    expect(1u, PLINK_FWK_RELAY, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    TEST_ASSERT_EQUAL_MEMORY(W.run.image.sha256, sent(1u)->sha256, PLINK_SHA256_LEN);
    TEST_ASSERT_EQUAL_UINT32(W.run.image.size, sent(1u)->size);
    says(1u, PLINK_FWS_RECEIVING, 0u);
    NOW += 1000u;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders()); /* once: the core resends it */
    for (size_t k = 0; k < PLINK_FWP_BUDGET; k++) {
        TEST_ASSERT_EQUAL_UINT64(0u, tr(1u)->fails[k]);
    }
}

/* LINK-031: a fetch that fails for a reach or transfer reason, or finds another version at its
 * check, falls back to the relay (UNKNOWN_VERSION: from the own slot); a refusal of the image
 * never does; a server's refusal blocks; a person's stop is STOPPED. */
static void test_a_fetch_falls_back_to_the_relay(void)
{
    static const struct {
        uint8_t why;
        uint8_t state, block;
        bool relay;
    } rows[] = {
        {PLINK_FW_WHY_NO_CONNECT, PLINK_FWK_RELAY, 0u, true},
        {PLINK_FW_WHY_BROKE_OFF, PLINK_FWK_RELAY, 0u, true},
        {PLINK_FW_WHY_BAD_SHA, PLINK_FWK_RELAY, 0u, true},
        {PLINK_FW_WHY_OTHER_VERSION, PLINK_FWK_RELAY, 0u, true},
        {PLINK_FW_WHY_UP_TO_DATE, PLINK_FWK_RELAY, 0u, true},
        {PLINK_FW_WHY_UNKNOWN_VERSION, PLINK_FWK_RELAY, 0u, true},
        {PLINK_FW_WHY_SIGNATURE, PLINK_FWK_FAILED, PLINK_FW_WHY_SIGNATURE, false},
        {PLINK_FW_WHY_WRONG_VERSION, PLINK_FWK_FAILED, PLINK_FW_WHY_WRONG_VERSION, false},
        {PLINK_FW_WHY_HELD_BACK, PLINK_FWK_BLOCKED, PLINK_FW_WHY_HELD_BACK, false},
        {PLINK_FW_WHY_NOT_OFFERED, PLINK_FWK_BLOCKED, PLINK_FW_WHY_NOT_OFFERED, false},
        {PLINK_FW_WHY_STOPPED, PLINK_FWK_STOPPED, PLINK_FW_WHY_STOPPED, false},
    };
    char msg[32];
    for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++) {
        setUp();
        keypad(1u, OLD);
        probed(1u, PLINK_FW_WHY_OK, V);
        says(1u, PLINK_FWS_FETCHING, 0u);
        NOW += 5000u;
        says(1u, PLINK_FWS_FAILED, rows[r].why);
        step();
        snprintf(msg, sizeof msg, "row %u", (unsigned)r);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(rows[r].state, tr(1u)->state, msg);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(rows[r].block, tr(1u)->why, msg);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(rows[r].relay ? PLINK_FW_OP_RELAY : 0u, sent(1u)->op, msg);
        TEST_ASSERT_EQUAL_MESSAGE(rows[r].relay, tr(1u)->fell_back, msg);
        /* A run that failed counts toward the BUDGET as the keypad counts it (an OTHER_VERSION
         * fetch too); a person's stop doesn't. */
        unsigned counted = 0u;
        for (size_t k = 0; k < PLINK_FWP_BUDGET; k++) {
            counted += tr(1u)->fails[k] != 0u ? 1u : 0u;
        }
        TEST_ASSERT_EQUAL_UINT_MESSAGE(rows[r].why == PLINK_FW_WHY_STOPPED ? 0u : 1u, counted, msg);
        if (rows[r].relay) {
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(PLINK_FWK_PATH_RELAY_OWN, tr(1u)->path, msg);
        }
    }
    /* Once per target: the relay fails too, and its retry's PROBE offers V again - the relay
     * again, not the fetch. */
    setUp();
    keypad(1u, OLD);
    probed(1u, PLINK_FW_WHY_OK, V);
    says(1u, PLINK_FWS_FAILED, PLINK_FW_WHY_BROKE_OFF);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    says(1u, PLINK_FWS_FAILED, PLINK_FW_WHY_STALLED);
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FW_WHY_STALLED);
    NOW += PLINK_FWP_RETRY_MS;
    probed(1u, PLINK_FW_WHY_OK, V);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
}

/* LINK-035, decided 2026-10-17: a transfer failure is tried again after 2 min; after 3 failed runs
 * in 24 h (the keypad's BUDGET) once those 24 h have passed - the keypad's own limit, which Try
 * again now neither offers nor ends. A BUDGET refusal is tried again once the 3 runs this master
 * knows of are a day old, else hourly (a master restart forgot them; a refused order costs
 * nothing), and Try again now doesn't override it. */
static void test_a_transfer_failure_is_tried_again(void)
{
    keypad(1u, OLD)->info.can = PLINK_FWI_RELAY;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    says(1u, PLINK_FWS_RECEIVING, 0u);
    const uint64_t first = NOW + 70000u;
    for (unsigned n = 0; n < PLINK_FWP_BUDGET; n++) {
        NOW += 70000u;
        says(1u, PLINK_FWS_FAILED, PLINK_FW_WHY_STALLED);
        step();
        expect(1u, PLINK_FWK_FAILED, PLINK_FW_WHY_STALLED);
        TEST_ASSERT_EQUAL_HEX8(n + 1u < PLINK_FWP_BUDGET ? 0x01u : 0x00u, S.held);
        if (n + 1u == PLINK_FWP_BUDGET) {
            break;
        }
        NOW += PLINK_FWP_RETRY_MS - 1u;
        step();
        TEST_ASSERT_EQUAL_UINT(0u, orders());
        NOW += 1u;
        step();
        TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
        expect(1u, PLINK_FWK_RELAY, 0u);
        says(1u, PLINK_FWS_RECEIVING, 0u);
    }
    /* Three in 24 h: the next once the first is a day old - Try again now doesn't bring it
     * sooner, nor later. */
    TEST_ASSERT_EQUAL_UINT64(first + PLINK_FWP_BUDGET_MS, tr(1u)->retry_at);
    TEST_ASSERT_TRUE(tr(1u)->budget);
    NOW += 1000u;
    W.again_ms[0] = NOW;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    expect(1u, PLINK_FWK_FAILED, PLINK_FW_WHY_STALLED);
    TEST_ASSERT_EQUAL_UINT64(first + PLINK_FWP_BUDGET_MS, tr(1u)->retry_at);
    TEST_ASSERT_EQUAL_HEX8(0u, S.held);
    NOW = first + PLINK_FWP_BUDGET_MS - 1u;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    NOW += 1u;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    /* The keypad says BUDGET (its own count, one more than this master knows of in 24 h): asked
     * again hourly; Try again now doesn't end it. */
    says(1u, PLINK_FWS_IDLE, PLINK_FW_WHY_BUDGET);
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FW_WHY_BUDGET);
    TEST_ASSERT_EQUAL_UINT64(NOW + PLINK_FWP_BUDGET_ASK_MS, tr(1u)->retry_at);
    TEST_ASSERT_EQUAL_HEX8(0u, S.held);
    const uint64_t refused_at = NOW;
    NOW += 1000u;
    W.again_ms[0] = NOW;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    expect(1u, PLINK_FWK_FAILED, PLINK_FW_WHY_BUDGET);
    NOW = refused_at + PLINK_FWP_BUDGET_ASK_MS;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    /* A BUDGET refusal while this master knows of 3 runs in 24 h: once the oldest is a day old. */
    setUp();
    keypad(1u, OLD)->info.can = PLINK_FWI_RELAY;
    tr(1u)->fails[0] = NOW - 30u * MIN_MS;
    tr(1u)->fails[1] = NOW - 20u * MIN_MS;
    tr(1u)->fails[2] = NOW - 10u * MIN_MS;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    says(1u, PLINK_FWS_IDLE, PLINK_FW_WHY_BUDGET);
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FW_WHY_BUDGET);
    TEST_ASSERT_EQUAL_UINT64(NOW - 30u * MIN_MS + PLINK_FWP_BUDGET_MS, tr(1u)->retry_at);
}

/* LINK-035 (decided 2026-10-17): a relay that times out paused at its gate - its FW_STATE said
 * PAUSED, NOT_QUIET (armed) - isn't a failed run: four in a day count nothing, nothing is cancelled
 * while paused, and each is tried again 2 min on, once the master is quiet. One that times out
 * after a pause for NO_STATUS (the link gone) counts. */
static void test_a_timeout_at_the_gate_isnt_counted(void)
{
    keypad(1u, OLD)->info.can = PLINK_FWI_RELAY;
    for (unsigned n = 0; n < 4u; n++) {
        step();
        TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
        says(1u, PLINK_FWS_RECEIVING, 0u);
        step();
        W.quiet = false; /* armed */
        NOW += 1000u;
        says(1u, PLINK_FWS_RECEIVING, PLINK_FW_WHY_NOT_QUIET)->flags = PLINK_FWF_PAUSED;
        step();
        expect(1u, PLINK_FWK_RELAY, 0u);
        TEST_ASSERT_EQUAL_UINT(0u, orders());
        NOW += 30u * MIN_MS;
        says(1u, PLINK_FWS_FAILED, PLINK_FW_WHY_TIMEOUT);
        step();
        expect(1u, PLINK_FWK_FAILED, PLINK_FW_WHY_TIMEOUT);
        for (size_t k = 0; k < PLINK_FWP_BUDGET; k++) {
            TEST_ASSERT_EQUAL_UINT64(0u, tr(1u)->fails[k]);
        }
        NOW += PLINK_FWP_RETRY_MS;
        step();
        TEST_ASSERT_EQUAL_UINT(0u, orders()); /* still armed */
        W.quiet = true;
        NOW += 1000u;
    }
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    says(1u, PLINK_FWS_RECEIVING, 0u);
    step();
    NOW += 15000u; /* no STATUS for 15 s */
    says(1u, PLINK_FWS_RECEIVING, PLINK_FW_WHY_NO_STATUS)->flags = PLINK_FWF_PAUSED;
    step();
    NOW += 30u * MIN_MS;
    says(1u, PLINK_FWS_FAILED, PLINK_FW_WHY_TIMEOUT);
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FW_WHY_TIMEOUT);
    unsigned counted = 0u;
    for (size_t k = 0; k < PLINK_FWP_BUDGET; k++) {
        counted += tr(1u)->fails[k] != 0u ? 1u : 0u;
    }
    TEST_ASSERT_EQUAL_UINT(1u, counted);
    TEST_ASSERT_EQUAL_UINT64(NOW + PLINK_FWP_RETRY_MS, tr(1u)->retry_at);
}

/* LINK-030: a keypad sends FW_INFO once a session, so one can be lost. Told to install, keypad 1
 * restarts into the target unseen (its INSTALLING lost) and is back in a new session without its
 * FW_INFO: it isn't judged on the facts of before - it keeps the install turn (keypad 2, READY,
 * waits), is asked INFO (the core resends that one until it is answered: no other goes over it),
 * and 10 min on is neither NO_RETURN nor its model held. Once its FW_INFO comes it is on its
 * self-test, then UPDATED, and keypad 2 gets the turn. */
static void test_a_lost_fw_info_after_a_restart_is_asked_for(void)
{
    keypad(1u, OLD)->info.can = PLINK_FWI_RELAY;
    keypad(2u, OLD)->info.can = PLINK_FWI_RELAY;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    says(1u, PLINK_FWS_RECEIVING, 0u);
    NOW += 20000u;
    ready(1u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(2u)->op);
    says(1u, PLINK_FWS_READY, 0u); /* the INSTALL echoed */
    says(2u, PLINK_FWS_RECEIVING, 0u);
    NOW += 20000u;
    ready(2u);
    step();
    expect(2u, PLINK_FWK_READY, 0u);
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    /* Keypad 1 restarted: a new session, its FW_INFO and FW_STATE there lost. */
    kp(1u)->sessions++;
    kp(1u)->info_now = false;
    kp(1u)->state_now = false;
    NOW += 6000u;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(2u)->op);
    expect(1u, PLINK_FWK_READY, 0u);
    NOW += 5000u;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    NOW += 5000u;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders()); /* the core resends it: still unanswered */
    NOW += PLINK_FWP_RETURN_MS;
    kp(2u)->state_ms = NOW; /* keypad 2's FW_STATE, every 10 s while READY */
    step();
    expect(1u, PLINK_FWK_READY, 0u);
    TEST_ASSERT_TRUE(tr(1u)->installed);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(2u)->op);
    TEST_ASSERT_EQUAL_CHAR('\0', W.holds.canary[0].device[0]);
    /* Its FW_INFO on the target, and its FW_STATE on probation. */
    plink_fw_kp_t *k = kp(1u);
    k->info_now = true;
    str(k->info.version, sizeof k->info.version, V);
    memset(&k->state, 0, sizeof k->state);
    k->state.order = k->order;
    k->state.op = k->order_op;
    k->state.probation = 1u;
    k->state_now = true;
    k->state_ms = NOW;
    k->order_acked = true;
    step();
    expect(1u, PLINK_FWK_CHECKING, 0u);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(2u)->op);
    NOW += 30000u;
    k->state.probation = 0u;
    k->state.last = PLINK_FW_LAST_UPDATED;
    str(k->state.last_version, sizeof k->state.last_version, V);
    k->state_ms = kp(2u)->state_ms = NOW;
    step();
    expect(1u, PLINK_FWK_UPDATED, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(2u)->op);
}

/* LINK-030, LINK-035: a run's end goes once. Keypad 1's relay is stopped at the keypad and that
 * FW_STATE is lost: heard, but its run's FW_STATE (every 10 s) a minute late, it is asked INFO -
 * and sent no RELAY meanwhile (that would start the stopped run again); the answer ends the run
 * (the stop held 24 h), and the relay's turn goes to keypad 2. Keypad 2's FW_STATEs lost a minute
 * while its relay goes on: asked, and once it answers RECEIVING its RELAY goes again (INFO
 * superseded it in the core). */
static void test_a_lost_end_of_run_is_asked_for(void)
{
    keypad(1u, OLD)->info.can = PLINK_FWI_RELAY;
    keypad(2u, OLD)->info.can = PLINK_FWI_RELAY;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    says(1u, PLINK_FWS_RECEIVING, 0u);
    step();
    NOW += PLINK_FWP_LATE_MS - 1u;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    NOW += 1u;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(1u)->op);
    expect(1u, PLINK_FWK_RELAY, 0u);
    NOW += 2000u;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders()); /* no RELAY before the answer */
    says(1u, PLINK_FWS_STOPPED, PLINK_FW_WHY_STOPPED);
    step();
    expect(1u, PLINK_FWK_STOPPED, PLINK_FW_WHY_STOPPED);
    TEST_ASSERT_TRUE(W.holds.stop_until_ms[0] != 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(2u)->op);
    plink_fw_state_t *s2 = says(2u, PLINK_FWS_RECEIVING, 0u);
    s2->size = W.run.image.size;
    memcpy(s2->id8, W.run.image.sha256, PLINK_ID8_LEN);
    step();
    NOW += PLINK_FWP_LATE_MS;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(2u)->op);
    NOW += 1000u;
    s2 = says(2u, PLINK_FWS_RECEIVING, 0u);
    s2->size = W.run.image.size;
    memcpy(s2->id8, W.run.image.sha256, PLINK_ID8_LEN);
    step();
    expect(2u, PLINK_FWK_RELAY, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(2u)->op);
    TEST_ASSERT_EQUAL_MEMORY(W.run.image.sha256, sent(2u)->sha256, PLINK_SHA256_LEN);
    expect(1u, PLINK_FWK_STOPPED, PLINK_FW_WHY_STOPPED);
}

/* LINK-030, LINK-031: a keypad found behind in a new session whose FW_INFO was lost has facts from
 * an earlier session only: it is asked INFO before it is probed on them - once while that INFO is
 * unanswered (the core resends it), again 10 s on once it was answered without a FW_INFO. */
static void test_a_keypad_on_old_facts_is_asked_info_first(void)
{
    keypad(1u, OLD)->info_now = false;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(1u)->op);
    expect(1u, PLINK_FWK_BEHIND, 0u);
    NOW += 5000u;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    NOW += PLINK_FWP_INFO_MS;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders()); /* unanswered: the core resends it */
    says(1u, PLINK_FWS_IDLE, 0u);         /* answered, its FW_INFO lost again */
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(1u)->op);
    kp(1u)->info_now = true;
    NOW += 1000u;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
    expect(1u, PLINK_FWK_PROBING, 0u);
}

/* LINK-035: a run lost is a transfer failure - a relay is tried again, a fetch falls back to the
 * relay: its keypad heard in a new session with no FW_STATE there is asked (INFO), and 30 s on its
 * run counts as gone; one whose FW_STATE there says it knows no order and runs nothing (it
 * restarted) at once. A relay whose keypad is unheard for 31 min gives the turn back (its own run
 * has timed out). */
static void test_a_lost_run_is_tried_again(void)
{
    keypad(1u, OLD)->info.can = PLINK_FWI_RELAY;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    says(1u, PLINK_FWS_RECEIVING, 0u);
    step();
    expect(1u, PLINK_FWK_RELAY, 0u);
    NOW += 40000u;
    back(1u, OLD, false, 0u, 0u); /* a power cut: back without its run */
    step();
    expect(1u, PLINK_FWK_RELAY, 0u); /* not yet: its FW_STATE may still come */
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(1u)->op);
    NOW += PLINK_FWP_MOVED_MS - 1u;
    step();
    expect(1u, PLINK_FWK_RELAY, 0u);
    NOW += 1u;
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FW_WHY_BROKE_OFF);
    NOW += PLINK_FWP_RETRY_MS;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    /* Back with a FW_STATE that knows no order and runs nothing: gone at once. */
    says(1u, PLINK_FWS_RECEIVING, 0u);
    step();
    NOW += 5000u;
    back(1u, OLD, true, 0u, 0u);
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FW_WHY_BROKE_OFF);
    /* A fetch lost the same way. */
    setUp();
    keypad(1u, OLD);
    probed(1u, PLINK_FW_WHY_OK, V);
    says(1u, PLINK_FWS_FETCHING, 0u);
    step();
    NOW += 40000u;
    back(1u, OLD, false, 0u, 0u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(1u)->op);
    NOW += PLINK_FWP_MOVED_MS;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    TEST_ASSERT_TRUE(tr(1u)->fell_back);
    /* A relay whose keypad goes quiet. */
    setUp();
    keypad(1u, OLD)->info.can = PLINK_FWI_RELAY;
    keypad(2u, OLD)->info.can = PLINK_FWI_RELAY;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    says(1u, PLINK_FWS_RECEIVING, 0u);
    kp(1u)->online = false;
    kp(1u)->away = true;
    step();
    NOW += PLINK_FWP_RELAY_AWAY_MS - 1u;
    step();
    expect(1u, PLINK_FWK_RELAY, 0u);
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    NOW += 1u;
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FW_WHY_TIMEOUT);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(2u)->op);
}

/* LINK-032, LINK-035: a relay going on in a new session (a rekey, or the master's REKEY after a
 * damaged frame) whose first FW_STATE there is lost isn't a run that broke off - nor when the link
 * is cut just then for 45 s (its 30 s count only while it is heard): its keypad is asked (INFO);
 * the answer says RECEIVING, and its RELAY goes again (INFO superseded it), the same image - it
 * continues by id8. */
static void test_a_relay_in_a_new_session_is_asked_not_dropped(void)
{
    keypad(1u, OLD)->info.can = PLINK_FWI_RELAY;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    plink_fw_state_t *s = says(1u, PLINK_FWS_RECEIVING, 0u);
    s->size = W.run.image.size;
    memcpy(s->id8, W.run.image.sha256, PLINK_ID8_LEN);
    step();
    NOW += 2000u; /* a new session, its first FW_STATE there not in yet */
    kp(1u)->sessions++;
    kp(1u)->info_now = true;
    kp(1u)->state_now = false;
    step();
    expect(1u, PLINK_FWK_RELAY, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(1u)->op);
    /* The link cut for 45 s just then: unheard, its FW_STATEs missing (away). */
    kp(1u)->online = false;
    kp(1u)->away = true;
    for (unsigned n = 0; n < 9u; n++) {
        NOW += 5000u;
        step();
        expect(1u, PLINK_FWK_RELAY, 0u);
    }
    kp(1u)->online = true;
    step();
    expect(1u, PLINK_FWK_RELAY, 0u); /* back: not taken as broken off */
    TEST_ASSERT_EQUAL_UINT(0u, orders()); /* its INFO, still unanswered, is the core's to resend */
    NOW += 1000u;
    kp(1u)->away = false;
    s = says(1u, PLINK_FWS_RECEIVING, 0u);
    s->size = W.run.image.size;
    memcpy(s->id8, W.run.image.sha256, PLINK_ID8_LEN);
    step();
    expect(1u, PLINK_FWK_RELAY, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    TEST_ASSERT_EQUAL_MEMORY(W.run.image.sha256, sent(1u)->sha256, PLINK_SHA256_LEN);
    for (size_t k = 0; k < PLINK_FWP_BUDGET; k++) {
        TEST_ASSERT_EQUAL_UINT64(0u, tr(1u)->fails[k]);
    }
}

/* LINK-035: an order the keypad refuses fails that keypad for this target - a PROBE of a kind it
 * doesn't take, an INSTALL it can't read - until its facts change (no retry by time, however long
 * and however often the master checks). */
static void test_refusals_wait_for_new_facts(void)
{
    keypad(1u, OLD);
    step();
    says(1u, PLINK_FWS_IDLE, PLINK_FW_WHY_UNKNOWN_KIND);
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FW_WHY_UNKNOWN_KIND);
    TEST_ASSERT_EQUAL_HEX8(0x01u, S.held);
    for (unsigned h = 0; h < 72u; h++) {
        NOW += HOUR_MS;
        W.check_ms = NOW;
        step();
        TEST_ASSERT_EQUAL_UINT(0u, orders());
    }
    kp(1u)->info.accepted_key8[0] = 0x42u; /* a key accepted at its console */
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
    setUp();
    keypad(1u, OLD);
    probed(1u, PLINK_FW_WHY_OK, V);
    ready(1u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op);
    says(1u, PLINK_FWS_READY, PLINK_FW_WHY_BAD_ORDER);
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FW_WHY_BAD_ORDER);
    TEST_ASSERT_FALSE(S.holds_changed); /* no canary: it never restarted */
}

/* Decided 2026-10-17: a blocked keypad is probed again every 24 h, when the master's own check is
 * answered, and when its facts change; one with no image (NO_IMAGE) when the master reaches its
 * server again - a copy then. */
static void test_a_blocked_keypad_is_probed_again(void)
{
    keypad(1u, OLD);
    probed(1u, PLINK_FW_WHY_HELD_BACK, "");
    expect(1u, PLINK_FWK_BLOCKED, PLINK_FW_WHY_HELD_BACK);
    TEST_ASSERT_EQUAL_HEX8(0x01u, S.held);
    NOW += PLINK_FWP_HOLD_MS - 1u;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    NOW += 1u;
    probed(1u, PLINK_FW_WHY_HELD_BACK, "");
    expect(1u, PLINK_FWK_BLOCKED, PLINK_FW_WHY_HELD_BACK);
    /* The master's check is answered. */
    NOW += 1000u;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    W.check_ms = NOW;
    probed(1u, PLINK_FW_WHY_HELD_BACK, "");
    /* Its facts change (another board revision). */
    NOW += 1000u;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    str(kp(1u)->info.board_rev, sizeof kp(1u)->info.board_rev, "1.1");
    probed(1u, PLINK_FW_WHY_OK, V);
    expect(1u, PLINK_FWK_FETCH, 0u);
    /* No image: the master can't reach its server for a 10.1"'s copy, until it can. */
    setUp();
    W.check_ms = 0u;
    keypad(1u, OLD);
    str(kp(1u)->info.device, sizeof kp(1u)->info.device, DEV10);
    probed(1u, PLINK_FW_WHY_NO_CONNECT, "");
    expect(1u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_NO_IMAGE);
    NOW += HOUR_MS;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    W.check_ms = NOW;
    probed(1u, PLINK_FW_WHY_NO_CONNECT, "");
    expect(1u, PLINK_FWK_RELAY_WAIT, 0u);
    TEST_ASSERT_EQUAL_UINT8(1u, S.copy_for);
}

/* OTA-027, decided 2026-10-17: a person's Stop at the keypad holds it for 24 h (kept in link/fwgo),
 * then it is tried again; the master's next version ends it sooner. */
static void test_a_stop_at_the_keypad_holds_24_h(void)
{
    keypad(1u, OLD);
    probed(1u, PLINK_FW_WHY_OK, V);
    says(1u, PLINK_FWS_FETCHING, 0u);
    NOW += 10000u;
    says(1u, PLINK_FWS_STOPPED, PLINK_FW_WHY_STOPPED);
    step();
    expect(1u, PLINK_FWK_STOPPED, PLINK_FW_WHY_STOPPED);
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    TEST_ASSERT_TRUE(S.holds_changed);
    TEST_ASSERT_EQUAL_STRING(V, S.holds.version);
    TEST_ASSERT_EQUAL_UINT64(NOW + PLINK_FWP_HOLD_MS, S.holds.stop_until_ms[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01u, S.held);
    const uint64_t at = NOW;
    NOW = at + PLINK_FWP_HOLD_MS - 1u;
    W.check_ms = NOW;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    NOW += 1u;
    step();
    TEST_ASSERT_TRUE(S.holds_changed);
    TEST_ASSERT_EQUAL_UINT64(0u, S.holds.stop_until_ms[0]);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
    /* Stopped again; the master's next version ends it at once. */
    says(1u, PLINK_FWS_PROBING, 0u);
    NOW += 1500u;
    answer_probe(1u, PLINK_FW_WHY_OK, V);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_FETCH, sent(1u)->op);
    says(1u, PLINK_FWS_FETCHING, 0u);
    says(1u, PLINK_FWS_STOPPED, PLINK_FW_WHY_STOPPED);
    step();
    expect(1u, PLINK_FWK_STOPPED, PLINK_FW_WHY_STOPPED);
    NOW += HOUR_MS;
    master_on(NEXT);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
    TEST_ASSERT_EQUAL_STRING(NEXT, sent(1u)->version);
}

/* OTA-027 at the master (plink_fw_stop): CANCEL once to each keypad with a run not yet installing
 * (all, or the one named); one installing goes on to UPDATED; the stop ends after 24 h. */
static void test_a_stop_at_the_master_cancels(void)
{
    keypad(1u, OLD)->info.can = PLINK_FWI_RELAY;
    keypad(2u, OLD);
    keypad(3u, OLD);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    says(1u, PLINK_FWS_RECEIVING, 0u);
    says(2u, PLINK_FWS_PROBING, 0u);
    says(3u, PLINK_FWS_PROBING, 0u);
    NOW += 1500u;
    answer_probe(2u, PLINK_FW_WHY_OK, V);
    answer_probe(3u, PLINK_FW_WHY_OK, V);
    step();
    says(2u, PLINK_FWS_FETCHING, 0u);
    ready(3u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(3u)->op);
    says(3u, PLINK_FWS_INSTALLING, 0u);
    step();
    expect(3u, PLINK_FWK_INSTALLING, 0u);
    TEST_ASSERT_EQUAL_HEX8(0x07u, S.running);
    plink_fw_stop(&W.holds, 0u, V, NOW);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sent(2u)->op);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(3u)->op);
    expect(1u, PLINK_FWK_STOPPED, PLINK_FW_WHY_CANCELLED);
    expect(2u, PLINK_FWK_STOPPED, PLINK_FW_WHY_CANCELLED);
    expect(3u, PLINK_FWK_INSTALLING, 0u);
    TEST_ASSERT_EQUAL_HEX8(0x04u, S.running);
    TEST_ASSERT_EQUAL_HEX8(0x03u, S.held);
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders()); /* once: the core resends it */
    says(1u, PLINK_FWS_STOPPED, PLINK_FW_WHY_CANCELLED);
    says(2u, PLINK_FWS_STOPPED, PLINK_FW_WHY_CANCELLED);
    back(3u, V, true, 0u, PLINK_FW_LAST_UPDATED);
    step();
    TEST_ASSERT_EQUAL_UINT(1u, orders());
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(3u)->op); /* its new session: V named */
    TEST_ASSERT_EQUAL_STRING(V, sent(3u)->version);
    expect(3u, PLINK_FWK_UPDATED, 0u);
    expect(1u, PLINK_FWK_STOPPED, PLINK_FW_WHY_CANCELLED);
    NOW += PLINK_FWP_HOLD_MS;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(2u)->op);
    /* One keypad only; a stop held for another version is replaced. */
    setUp();
    keypad(1u, OLD);
    keypad(2u, OLD);
    plink_fw_stop(&W.holds, 1u, OLD, NOW);
    plink_fw_stop(&W.holds, 2u, V, NOW);
    TEST_ASSERT_EQUAL_UINT64(0u, W.holds.stop_until_ms[0]);
    plink_fw_stop(&W.holds, 9u, V, NOW); /* no such keypad: nothing */
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(2u)->op);
    expect(2u, PLINK_FWK_STOPPED, PLINK_FW_WHY_CANCELLED);
    /* A removed keypad's stop goes with it. */
    kp(2u)->device = 0u;
    step();
    TEST_ASSERT_EQUAL_UINT64(0u, S.holds.stop_until_ms[1]);
    TEST_ASSERT_TRUE(S.holds_changed);
    /* A READY keypad holding an INSTALL from before a master restart is stopped too. */
    setUp();
    keypad(1u, OLD);
    says(1u, PLINK_FWS_READY, 0u)->op = PLINK_FW_OP_INSTALL;
    plink_fw_stop(&W.holds, 1u, V, NOW);
    step();
    expect(1u, PLINK_FWK_STOPPED, PLINK_FW_WHY_CANCELLED);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sent(1u)->op);
    TEST_ASSERT_FALSE(tr(1u)->installed);
}

/* Decided 2026-10-17: Try again now ends the master's holds at once - a stop, a block, a refusal
 * held, a failure waiting, a model's canary - for the keypads it names; never a keypad's BUDGET;
 * a request is taken once. A canary's model starts with the keypad that went back (P2 decision 13:
 * its last section's rollback of before doesn't hold it again), the model's other keypads held
 * until it is UPDATED. */
static void test_try_again_now(void)
{
    for (uint8_t d = 1u; d <= 6u; d++) {
        keypad(d, OLD);
    }
    v3(5u);
    v3(6u);
    step();
    for (uint8_t d = 1u; d <= 6u; d++) {
        TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(d)->op);
    }
    NOW += 1500u;
    answer_probe(1u, PLINK_FW_WHY_OK, V);
    answer_probe(2u, PLINK_FW_WHY_HELD_BACK, "");
    says(3u, PLINK_FWS_IDLE, PLINK_FW_WHY_UNKNOWN_KIND);
    says(4u, PLINK_FWS_IDLE, PLINK_FW_WHY_BUDGET);
    answer_probe(5u, PLINK_FW_WHY_OK, V);
    answer_probe(6u, PLINK_FW_WHY_OK, V);
    step();
    says(1u, PLINK_FWS_FETCHING, 0u);
    says(1u, PLINK_FWS_STOPPED, PLINK_FW_WHY_STOPPED);
    ready(5u);
    ready(6u);
    step();
    installs(5u);
    back(5u, OLD, true, 0u, PLINK_FW_LAST_ROLLED_BACK);
    step();
    expect(1u, PLINK_FWK_STOPPED, PLINK_FW_WHY_STOPPED);
    expect(2u, PLINK_FWK_BLOCKED, PLINK_FW_WHY_HELD_BACK);
    expect(3u, PLINK_FWK_FAILED, PLINK_FW_WHY_UNKNOWN_KIND);
    expect(4u, PLINK_FWK_FAILED, PLINK_FW_WHY_BUDGET);
    expect(5u, PLINK_FWK_FAILED, PLINK_FWK_WHY_ROLLED_BACK);
    expect(6u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
    says(6u, PLINK_FWS_STOPPED, PLINK_FW_WHY_CANCELLED);
    NOW += 1000u;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    TEST_ASSERT_EQUAL_HEX8(0x37u, S.held); /* all but keypad 4 (BUDGET) */
    /* For keypad 2 only. */
    W.again_ms[1] = NOW;
    step();
    TEST_ASSERT_EQUAL_UINT(1u, orders());
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(2u)->op);
    NOW += 1000u;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders()); /* taken once */
    /* For all. */
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        W.again_ms[i] = NOW;
    }
    step();
    TEST_ASSERT_TRUE(S.holds_changed);
    TEST_ASSERT_EQUAL_UINT64(0u, S.holds.stop_until_ms[0]);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(3u)->op);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(4u)->op);
    expect(4u, PLINK_FWK_FAILED, PLINK_FW_WHY_BUDGET);
    TEST_ASSERT_EQUAL_STRING("v3", S.holds.canary[0].chip_rev);
    TEST_ASSERT_EQUAL_UINT8(5u, S.holds.canary[0].retry);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(5u)->op);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(6u)->op);
    expect(6u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
    /* Keypad 5's PROBE echo still says it went back from V: that is what it tries again. */
    plink_fw_state_t *s = says(5u, PLINK_FWS_PROBING, 0u);
    s->last = PLINK_FW_LAST_ROLLED_BACK;
    str(s->last_version, sizeof s->last_version, V);
    NOW += 1500u;
    answer_probe(5u, PLINK_FW_WHY_OK, V);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_FETCH, sent(5u)->op);
    ready(5u);
    step();
    installs(5u);
    expect(6u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
    back(5u, V, true, 0u, PLINK_FW_LAST_UPDATED);
    step();
    expect(5u, PLINK_FWK_UPDATED, 0u);
    TEST_ASSERT_EQUAL_STRING("", S.holds.canary[0].device);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(6u)->op); /* its model's hold over */
}

/* P2 decision 13 at the console (plink fw update n): Try again now for another keypad of a held
 * model still starts with the one that went back; one that goes back again holds the whole model
 * again, until the master's next version or another Try again now. */
static void test_try_again_now_starts_with_the_canary(void)
{
    for (uint8_t d = 1u; d <= 3u; d++) {
        keypad(d, OLD);
    }
    all_fetching(3u);
    for (uint8_t d = 1u; d <= 3u; d++) {
        ready(d);
    }
    step();
    installs(1u);
    back(1u, OLD, true, 0u, PLINK_FW_LAST_ROLLED_BACK);
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_ROLLED_BACK);
    expect(2u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
    expect(3u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
    says(2u, PLINK_FWS_STOPPED, PLINK_FW_WHY_CANCELLED);
    says(3u, PLINK_FWS_STOPPED, PLINK_FW_WHY_CANCELLED);
    NOW += 1000u;
    W.again_ms[2] = NOW; /* plink fw update 3 */
    step();
    TEST_ASSERT_EQUAL_UINT8(1u, S.holds.canary[0].retry);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(2u)->op);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(3u)->op);
    expect(3u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
    says(1u, PLINK_FWS_PROBING, 0u);
    NOW += 1500u;
    answer_probe(1u, PLINK_FW_WHY_OK, V);
    step();
    ready(1u);
    step();
    installs(1u);
    back(1u, OLD, true, 0u, PLINK_FW_LAST_ROLLED_BACK); /* back again */
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_ROLLED_BACK);
    TEST_ASSERT_EQUAL_UINT8(0u, S.holds.canary[0].retry);
    TEST_ASSERT_EQUAL_STRING(DEVICE, S.holds.canary[0].device);
    for (unsigned h = 0; h < 48u; h++) {
        NOW += HOUR_MS;
        W.check_ms = NOW;
        step();
        TEST_ASSERT_EQUAL_UINT8(0u, sent(1u)->op);
        TEST_ASSERT_EQUAL_UINT8(0u, sent(2u)->op);
        TEST_ASSERT_EQUAL_UINT8(0u, sent(3u)->op);
    }
}

/* Decided 2026-10-17, P2 decision 13 across a master restart: the canary kept (link/fwgo), the
 * keypad that went back tells it again in its FW_STATE's last section (its INFO's answer); Try
 * again now then sends it on - its PROBE's echo, still saying it went back from V, doesn't bring
 * the hold back - and it is probed, fetched and installed. The same when Try again now comes in
 * the minute after the start, before that keypad said anything. */
static void test_try_again_now_after_a_master_restart(void)
{
    for (unsigned variant = 0; variant < 2u; variant++) {
        setUp();
        keypad(1u, OLD);
        keypad(2u, OLD);
        all_fetching(2u);
        ready(1u);
        ready(2u);
        step();
        installs(1u);
        back(1u, OLD, true, 0u, PLINK_FW_LAST_ROLLED_BACK);
        step();
        expect(2u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
        says(2u, PLINK_FWS_STOPPED, PLINK_FW_WHY_CANCELLED);
        step();
        master_restart();
        if (variant == 1u) {
            NOW += 1000u;
            W.again_ms[0] = NOW;
            step();
            TEST_ASSERT_EQUAL_UINT(0u, orders());
        }
        NOW = W.start_ms + PLINK_FWP_SETTLE_MS;
        step();
        TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(2u)->op); /* its new session: V named */
        expect(2u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
        says(2u, PLINK_FWS_IDLE, 0u);
        plink_fw_state_t *s = NULL;
        if (variant == 0u) {
            TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(1u)->op);
            s = says(1u, PLINK_FWS_IDLE, 0u);
            s->last = PLINK_FW_LAST_ROLLED_BACK;
            str(s->last_version, sizeof s->last_version, V);
            step();
            expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_ROLLED_BACK);
            NOW += 1000u;
            W.again_ms[0] = NOW;
            step();
        }
        TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
        TEST_ASSERT_FALSE(starts(2u));
        s = says(1u, PLINK_FWS_PROBING, 0u);
        s->last = PLINK_FW_LAST_ROLLED_BACK;
        str(s->last_version, sizeof s->last_version, V);
        step();
        expect(1u, PLINK_FWK_PROBING, 0u); /* its rollback of before holds nothing now */
        expect(2u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
        NOW += 1500u;
        answer_probe(1u, PLINK_FW_WHY_OK, V);
        step();
        expect(1u, PLINK_FWK_FETCH, 0u);
        TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_FETCH, sent(1u)->op);
        ready(1u);
        step();
        TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op);
        installs(1u);
        back(1u, V, true, 0u, PLINK_FW_LAST_UPDATED);
        step();
        expect(1u, PLINK_FWK_UPDATED, 0u);
        TEST_ASSERT_EQUAL_STRING("", S.holds.canary[0].device);
        TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(2u)->op);
    }
}

/* Decided 2026-10-17: the holds outlive a master restart (link/fwgo): after it, a keypad held as
 * the canary and one stopped get nothing but INFO naming V (their new sessions); a READY one is
 * cancelled - its INSTALL of before went with its session. Without the holds, a keypad that went
 * back from the target says so in its FW_STATE's last section (its PROBE's echo): held again,
 * never installed. The holds of the version a master is on its self-test of are kept until it is
 * confirmed on another. */
static void test_holds_outlive_a_master_restart(void)
{
    keypad(1u, OLD);
    keypad(2u, OLD);
    keypad(3u, OLD);
    v3(3u);
    all_fetching(3u);
    ready(1u);
    step();
    installs(1u);
    back(1u, OLD, true, 0u, PLINK_FW_LAST_ROLLED_BACK);
    step();
    TEST_ASSERT_EQUAL_STRING(DEVICE, S.holds.canary[0].device);
    says(3u, PLINK_FWS_STOPPED, PLINK_FW_WHY_STOPPED);
    step();
    TEST_ASSERT_NOT_EQUAL(0u, S.holds.stop_until_ms[2]);
    const plink_fw_holds_t kept = W.holds;
    /* The master restarts: its keypads report their facts again; keypad 2 still holds its READY
     * image (the INSTALL it echoes went with its session) - cancelled, never installed. */
    master_restart();
    says(2u, PLINK_FWS_READY, 0u)->op = PLINK_FW_OP_INSTALL;
    NOW += PLINK_FWP_SETTLE_MS;
    step();
    TEST_ASSERT_EQUAL_UINT(3u, orders());
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sent(2u)->op);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(3u)->op);
    says(1u, PLINK_FWS_IDLE, 0u); /* its last section still: it went back from V */
    says(2u, PLINK_FWS_STOPPED, PLINK_FW_WHY_CANCELLED);
    says(3u, PLINK_FWS_IDLE, 0u);
    step();
    TEST_ASSERT_EQUAL_UINT(1u, orders());
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(2u)->op); /* (its CANCEL named none) */
    says(2u, PLINK_FWS_STOPPED, PLINK_FW_WHY_CANCELLED);
    for (unsigned n = 0; n < 10u; n++) {
        step();
        TEST_ASSERT_EQUAL_UINT(0u, orders());
        NOW += MIN_MS;
    }
    expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_ROLLED_BACK);
    expect(2u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
    expect(3u, PLINK_FWK_STOPPED, PLINK_FW_WHY_CANCELLED);
    TEST_ASSERT_FALSE(S.holds_changed);
    /* Holds lost too: keypad 1's PROBE echo carries its rollback of V. */
    master_restart();
    memset(&W.holds, 0, sizeof W.holds);
    NOW += PLINK_FWP_SETTLE_MS;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
    plink_fw_state_t *s = says(1u, PLINK_FWS_PROBING, 0u);
    s->last = PLINK_FW_LAST_ROLLED_BACK;
    str(s->last_version, sizeof s->last_version, V);
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_ROLLED_BACK);
    expect(2u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
    TEST_ASSERT_TRUE(S.holds_changed);
    TEST_ASSERT_EQUAL_STRING(DEVICE, S.holds.canary[0].device);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(2u)->op); /* its PROBE (no run to cancel): never a FETCH */
    /* The master on its self-test of NEXT: V's holds stay (it may go back to V). */
    W.holds = kept;
    master_restart();
    str(W.run.image.version, sizeof W.run.image.version, NEXT);
    W.own = PLINK_FWM_SELF_TEST;
    step();
    TEST_ASSERT_FALSE(S.holds_changed);
    TEST_ASSERT_EQUAL_STRING(V, S.holds.version);
    W.own = PLINK_FWM_IDLE;
    step();
    TEST_ASSERT_TRUE(S.holds_changed);
    TEST_ASSERT_EQUAL_STRING("", S.holds.version);
}

/* LINK-030, decided 2026-10-17: no automatic loop restarts a keypad. A keypad that rolls back
 * every time is restarted once per master version - through three days of the master's hourly
 * checks and a master restart - and holds its model (another keypad of it is never restarted);
 * another model is updated; the master's next version tries the first once more. So is one whose
 * core speaks before its service: back on the target with probation over and nothing more said,
 * then back on its old version without a word - never taken for UPDATED, never tried again by
 * itself. A relay that stalls every time is bounded by the budget: at most 3 runs a day, no
 * restart. */
static void test_no_keypad_restarts_in_a_loop(void)
{
    keypad(1u, OLD);
    M[0].mode = M_ROLLBACK;
    keypad(2u, OLD);
    M[1].mode = M_ROLLBACK;
    keypad(3u, OLD);
    v3(3u);
    M[2].mode = M_GOOD;
    keypad(4u, OLD);
    str(kp(4u)->info.device, sizeof kp(4u)->info.device, DEV10); /* a model of its own */
    M[3].mode = M_LATE;
    run_model(36u * HOUR_MS, MIN_MS);
    master_restart();
    run_model(36u * HOUR_MS, MIN_MS);
    TEST_ASSERT_EQUAL_UINT(1u, M[0].ops[PLINK_FW_OP_INSTALL]);
    TEST_ASSERT_EQUAL_UINT(0u, M[1].ops[PLINK_FW_OP_INSTALL]);
    TEST_ASSERT_EQUAL_UINT(1u, M[2].ops[PLINK_FW_OP_INSTALL]);
    TEST_ASSERT_EQUAL_UINT(1u, M[3].ops[PLINK_FW_OP_INSTALL]);
    TEST_ASSERT_EQUAL_STRING(OLD, kp(4u)->info.version);
    TEST_ASSERT_EQUAL_STRING(V, kp(3u)->info.version);
    expect(3u, PLINK_FWK_CURRENT, 0u);
    TEST_ASSERT_TRUE(tr(1u)->state == PLINK_FWK_FAILED || tr(1u)->state == PLINK_FWK_BLOCKED);
    expect(2u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
    /* The master's next version: one more try each (keypad 1 is its model's canary again). */
    master_on(NEXT);
    run_model(3u * DAY_MS, MIN_MS);
    TEST_ASSERT_EQUAL_UINT(2u, M[0].ops[PLINK_FW_OP_INSTALL]);
    TEST_ASSERT_EQUAL_UINT(0u, M[1].ops[PLINK_FW_OP_INSTALL]);
    TEST_ASSERT_EQUAL_UINT(2u, M[2].ops[PLINK_FW_OP_INSTALL]);
    TEST_ASSERT_EQUAL_UINT(2u, M[3].ops[PLINK_FW_OP_INSTALL]);
    TEST_ASSERT_EQUAL_STRING(NEXT, kp(3u)->info.version);
    /* A relay that stalls every time. */
    setUp();
    keypad(1u, OLD)->info.can = PLINK_FWI_RELAY;
    M[0].mode = M_STALL;
    run_model(3u * DAY_MS, MIN_MS);
    TEST_ASSERT_TRUE(M[0].ops[PLINK_FW_OP_RELAY] >= 7u);
    TEST_ASSERT_TRUE(M[0].ops[PLINK_FW_OP_RELAY] <= 3u * PLINK_FWP_BUDGET + 1u);
    TEST_ASSERT_EQUAL_UINT(0u, M[0].ops[PLINK_FW_OP_INSTALL]);
}

/* docs/features/11 "Who gets orders": when the master's own update starts, CANCEL goes to every
 * keypad with a run that isn't installing yet, and the copy is freed; while the master merely
 * isn't quiet (armed) nothing is cancelled and nothing new goes - the keypads pause by HOLD. */
static void test_the_masters_own_update_cancels(void)
{
    keypad(1u, OLD)->info.can = PLINK_FWI_RELAY;
    keypad(2u, OLD);
    keypad(3u, OLD);
    keypad(4u, OLD);
    keypad(5u, OLD)->info.can = PLINK_FWI_RELAY;
    str(kp(5u)->info.device, sizeof kp(5u)->info.device, DEV10);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    expect(5u, PLINK_FWK_RELAY_WAIT, 0u);
    TEST_ASSERT_EQUAL_UINT8(5u, S.copy_for);
    says(1u, PLINK_FWS_RECEIVING, 0u);
    for (uint8_t d = 2u; d <= 4u; d++) {
        says(d, PLINK_FWS_PROBING, 0u);
    }
    NOW += 1500u;
    answer_probe(2u, PLINK_FW_WHY_OK, V);
    answer_probe(3u, PLINK_FW_WHY_OK, V);
    step();
    ready(2u);
    ready(3u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(2u)->op);
    says(2u, PLINK_FWS_INSTALLING, 0u);
    /* A copy for keypad 5 is in; the system is armed: nothing cancelled, nothing new. */
    plink_fw_copy_t *c = &W.copy;
    c->state = PLINK_FWCOPY_READY;
    str(c->device, sizeof c->device, DEV10);
    str(c->chip_rev, sizeof c->chip_rev, "v1");
    str(c->project, sizeof c->project, PROJECT);
    c->image = W.run.image;
    c->used_ms = NOW;
    W.quiet = false;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    TEST_ASSERT_FALSE(S.copy_free);
    expect(1u, PLINK_FWK_RELAY, 0u);
    expect(2u, PLINK_FWK_INSTALLING, 0u);
    expect(3u, PLINK_FWK_READY, 0u);
    expect(5u, PLINK_FWK_RELAY_WAIT, 0u);
    /* The master's own update starts. */
    W.quiet = true;
    W.own = PLINK_FWM_UPDATING;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(2u)->op);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sent(3u)->op);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(4u)->op); /* a probe is no run */
    TEST_ASSERT_EQUAL_UINT8(0u, sent(5u)->op);
    TEST_ASSERT_TRUE(S.copy_free);
    expect(1u, PLINK_FWK_BEHIND, 0u);
    expect(3u, PLINK_FWK_BEHIND, 0u);
    expect(4u, PLINK_FWK_BEHIND, 0u);
    expect(5u, PLINK_FWK_BEHIND, 0u);
    TEST_ASSERT_FALSE(S.holds_changed);
    /* A READY keypad already told to install (its gate still waiting) isn't installing yet:
     * cancelled too. */
    setUp();
    keypad(1u, OLD);
    probed(1u, PLINK_FW_WHY_OK, V);
    ready(1u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op);
    says(1u, PLINK_FWS_READY, PLINK_FW_WHY_NOT_QUIET);
    step();
    expect(1u, PLINK_FWK_READY, 0u);
    TEST_ASSERT_TRUE(tr(1u)->installed);
    W.own = PLINK_FWM_UPDATING;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sent(1u)->op);
    expect(1u, PLINK_FWK_BEHIND, 0u);
    TEST_ASSERT_FALSE(tr(1u)->installed);
}

/* After a master restart (fresh tracks): a relay of the running image is sent again with the same
 * image (it continues by id8), a fetch goes on by itself, READY waits for the install turn an
 * installing keypad holds, a relay of an image no longer here is cancelled and waits. */
static void test_runs_taken_up_after_a_master_restart(void)
{
    for (uint8_t d = 1u; d <= 5u; d++) {
        keypad(d, OLD);
        kp(d)->order = 0u; /* the core has no order outstanding after its restart */
    }
    plink_fw_state_t *s = says(1u, PLINK_FWS_RECEIVING, PLINK_FW_WHY_NO_STATUS);
    s->order = 0x1234u;
    s->op = PLINK_FW_OP_RELAY;
    s->size = W.run.image.size;
    memcpy(s->id8, W.run.image.sha256, PLINK_ID8_LEN);
    s = says(2u, PLINK_FWS_FETCHING, 0u);
    s->op = PLINK_FW_OP_FETCH;
    s = says(3u, PLINK_FWS_READY, 0u);
    s->op = PLINK_FW_OP_FETCH;
    s = says(4u, PLINK_FWS_INSTALLING, 0u);
    s->op = PLINK_FW_OP_INSTALL;
    s = says(5u, PLINK_FWS_RECEIVING, 0u);
    s->op = PLINK_FW_OP_RELAY;
    s->size = 1000u;
    memset(s->id8, 0x77, PLINK_ID8_LEN);
    step();
    expect(1u, PLINK_FWK_RELAY, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    TEST_ASSERT_EQUAL_MEMORY(W.run.image.sha256, sent(1u)->sha256, PLINK_SHA256_LEN);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWK_PATH_RELAY_OWN, tr(1u)->path);
    expect(2u, PLINK_FWK_FETCH, 0u);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(2u)->op);
    expect(3u, PLINK_FWK_READY, 0u);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(3u)->op);
    expect(4u, PLINK_FWK_INSTALLING, 0u);
    expect(5u, PLINK_FWK_RELAY_WAIT, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sent(5u)->op);
    /* Its relay goes on (the echo of the new order); keypad 4 comes back updated: 3 installs. */
    says(1u, PLINK_FWS_RECEIVING, 0u);
    back(4u, V, true, 0u, PLINK_FW_LAST_UPDATED);
    step();
    expect(1u, PLINK_FWK_RELAY, 0u);
    expect(4u, PLINK_FWK_UPDATED, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(3u)->op);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(1u)->op);
}

/* After a master restart a keypad's FW_STATE may be lost while its FW_INFO arrives: it is PROBEd as
 * if idle, and the PROBE's echo - every order is answered with the whole state - shows its run:
 * taken up then (a relay sent again, READY to its turn), the answer not waited for. */
static void test_a_run_taken_up_from_a_probe_echo(void)
{
    keypad(1u, OLD);
    keypad(2u, OLD);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(2u)->op);
    plink_fw_state_t *s = says(1u, PLINK_FWS_RECEIVING, PLINK_FW_WHY_OK);
    s->size = W.run.image.size;
    memcpy(s->id8, W.run.image.sha256, PLINK_ID8_LEN);
    s = says(2u, PLINK_FWS_READY, PLINK_FW_WHY_OK);
    s->size = W.run.image.size;
    memcpy(s->id8, W.run.image.sha256, PLINK_ID8_LEN);
    step();
    expect(1u, PLINK_FWK_RELAY, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWK_PATH_RELAY_OWN, tr(1u)->path);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    TEST_ASSERT_EQUAL_MEMORY(W.run.image.sha256, sent(1u)->sha256, PLINK_SHA256_LEN);
    expect(2u, PLINK_FWK_READY, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWK_PATH_RELAY_OWN, tr(2u)->path);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(2u)->op);
}

/* LINK-030: for a minute after the master starts nothing new is ordered, not even INFO; runs are
 * taken up from the keypads' FW_STATEs, a relay sent again at once. A keypad installing holds the
 * install turn; one READY whose FW_STATE still echoes an INSTALL of before holds none (that INSTALL
 * went with its session, LINK-034) - it installs at its turn. After the minute the others go on. */
static void test_the_minute_after_a_start(void)
{
    W.start_ms = NOW;
    keypad(1u, OLD);
    keypad(2u, OLD);
    plink_fw_state_t *s = says(2u, PLINK_FWS_READY, 0u);
    s->op = PLINK_FW_OP_INSTALL;
    keypad(3u, OLD);
    s = says(3u, PLINK_FWS_READY, 0u);
    s->op = PLINK_FW_OP_RELAY;
    keypad(4u, OLD);
    s = says(4u, PLINK_FWS_RECEIVING, 0u);
    s->op = PLINK_FW_OP_RELAY;
    s->size = W.run.image.size;
    memcpy(s->id8, W.run.image.sha256, PLINK_ID8_LEN);
    keypad(5u, OLD);
    kp(5u)->have_info = kp(5u)->info_now = false;
    keypad(6u, OLD);
    says(6u, PLINK_FWS_INSTALLING, 0u)->op = PLINK_FW_OP_INSTALL;
    step();
    TEST_ASSERT_EQUAL_UINT(1u, orders());
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(4u)->op); /* it mustn't stall */
    expect(1u, PLINK_FWK_BEHIND, 0u);
    expect(2u, PLINK_FWK_READY, 0u);
    TEST_ASSERT_FALSE(tr(2u)->installed);
    expect(3u, PLINK_FWK_READY, 0u);
    expect(4u, PLINK_FWK_RELAY, 0u);
    expect(5u, PLINK_FWK_UNKNOWN, 0u);
    expect(6u, PLINK_FWK_INSTALLING, 0u);
    TEST_ASSERT_TRUE(tr(6u)->installed);
    NOW += PLINK_FWP_SETTLE_MS - 1u;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    NOW += 1u;
    kp(2u)->state_ms = kp(3u)->state_ms = kp(4u)->state_ms = NOW; /* every 10 s with a run */
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(2u)->op); /* keypad 6 holds the turn */
    TEST_ASSERT_EQUAL_UINT8(0u, sent(3u)->op);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(5u)->op);
    /* Keypad 6 is back updated: keypad 2, the lowest READY, installs; then keypad 3. */
    back(6u, V, true, 0u, PLINK_FW_LAST_UPDATED);
    step();
    expect(6u, PLINK_FWK_UPDATED, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(2u)->op);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(3u)->op);
    says(2u, PLINK_FWS_INSTALLING, 0u);
    step();
    expect(2u, PLINK_FWK_INSTALLING, 0u);
    back(2u, V, true, 0u, PLINK_FW_LAST_UPDATED);
    step();
    expect(2u, PLINK_FWK_UPDATED, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(3u)->op);
}

/* LINK-030, LINK-034: after a master restart a keypad whose FW_INFO was lost and whose FW_STATE
 * still echoes an INSTALL of before holds no INSTALL - it went with its session - so it holds no
 * install turn either: it waits for its facts (INFO, after the minute), is READY with them and
 * installs at its turn like any other, followed as any install. */
static void test_an_install_of_before_holds_nothing(void)
{
    W.start_ms = NOW;
    keypad(1u, OLD);
    kp(1u)->have_info = kp(1u)->info_now = false;
    says(1u, PLINK_FWS_READY, 0u)->op = PLINK_FW_OP_INSTALL;
    keypad(2u, OLD);
    says(2u, PLINK_FWS_READY, 0u)->op = PLINK_FW_OP_RELAY;
    step();
    expect(1u, PLINK_FWK_UNKNOWN, 0u);
    TEST_ASSERT_FALSE(tr(1u)->installed);
    expect(2u, PLINK_FWK_READY, 0u);
    NOW += PLINK_FWP_SETTLE_MS;
    kp(1u)->state_ms = kp(2u)->state_ms = NOW;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(2u)->op); /* no turn held */
    kp(1u)->have_info = kp(1u)->info_now = true;
    says(1u, PLINK_FWS_READY, 0u);
    says(2u, PLINK_FWS_READY, 0u);
    step();
    expect(1u, PLINK_FWK_READY, 0u);
    TEST_ASSERT_FALSE(tr(1u)->installed);
    TEST_ASSERT_EQUAL_UINT(0u, orders()); /* keypad 2 holds the turn */
    says(2u, PLINK_FWS_INSTALLING, 0u);
    step();
    expect(2u, PLINK_FWK_INSTALLING, 0u);
    back(2u, V, true, 0u, PLINK_FW_LAST_UPDATED);
    step();
    expect(2u, PLINK_FWK_UPDATED, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op);
}

/* LINK-032: after a master restart a keypad whose FW_INFO was lost but whose relay of the running
 * image goes on (its FW_STATE: RECEIVING, that id8 and size) is sent the same RELAY again at once,
 * within the minute after the start, before it stalls; its facts are asked for once it is READY.
 * A relay of an image this master doesn't hold waits for them: asked INFO, again 10 s after each
 * answer that brought none. */
static void test_a_relay_taken_up_before_its_facts(void)
{
    W.start_ms = NOW;
    keypad(1u, OLD);
    kp(1u)->have_info = kp(1u)->info_now = false;
    plink_fw_state_t *s = says(1u, PLINK_FWS_RECEIVING, PLINK_FW_WHY_NO_STATUS);
    s->op = PLINK_FW_OP_RELAY;
    s->size = W.run.image.size;
    memcpy(s->id8, W.run.image.sha256, PLINK_ID8_LEN);
    keypad(2u, OLD);
    kp(2u)->have_info = kp(2u)->info_now = false;
    s = says(2u, PLINK_FWS_RECEIVING, 0u);
    s->op = PLINK_FW_OP_RELAY;
    s->size = 1000u;
    memset(s->id8, 0x77, PLINK_ID8_LEN);
    step();
    expect(1u, PLINK_FWK_RELAY, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWK_PATH_RELAY_OWN, tr(1u)->path);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    TEST_ASSERT_EQUAL_MEMORY(W.run.image.sha256, sent(1u)->sha256, PLINK_SHA256_LEN);
    TEST_ASSERT_EQUAL_UINT32(W.run.image.size, sent(1u)->size);
    expect(2u, PLINK_FWK_UNKNOWN, 0u);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(2u)->op);
    says(1u, PLINK_FWS_RECEIVING, 0u);
    NOW += PLINK_FWP_SETTLE_MS;
    kp(1u)->state_ms = NOW; /* its FW_STATE, every 10 s while it receives */
    for (unsigned n = 0; n < 3u; n++) {
        step();
        TEST_ASSERT_EQUAL_UINT8(0u, sent(1u)->op); /* no INFO while it receives */
        TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(2u)->op);
        says(2u, PLINK_FWS_RECEIVING, 0u); /* answered; its FW_INFO lost */
        NOW += PLINK_FWP_INFO_MS;
        says(1u, PLINK_FWS_RECEIVING, 0u);
    }
    ready(1u);
    step();
    expect(1u, PLINK_FWK_READY, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(1u)->op);
}

/* LINK-031: one copy at a time, fetched with the facts of the lowest keypad that needs one; the
 * relay from it once READY; freed when no keypad of its facts needs it, after 30 min unused, or
 * when the master's own update starts; a refusal for those facts blocks them, a copy that couldn't
 * be fetched leaves no image - until the master's next check. */
static void test_the_copy(void)
{
    keypad(1u, OLD)->info.can = PLINK_FWI_RELAY;
    str(kp(1u)->info.device, sizeof kp(1u)->info.device, DEV10);
    keypad(2u, OLD)->info.can = PLINK_FWI_RELAY;
    v3(2u);
    step();
    expect(1u, PLINK_FWK_RELAY_WAIT, 0u);
    expect(2u, PLINK_FWK_RELAY_WAIT, 0u);
    TEST_ASSERT_EQUAL_UINT8(1u, S.copy_for);
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    plink_fw_copy_t *c = &W.copy;
    c->state = PLINK_FWCOPY_FETCHING;
    str(c->device, sizeof c->device, DEV10);
    str(c->chip_rev, sizeof c->chip_rev, "v1");
    str(c->project, sizeof c->project, PROJECT);
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, S.copy_for); /* one at a time */
    TEST_ASSERT_FALSE(S.copy_free);
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    c->state = PLINK_FWCOPY_READY;
    c->image = W.run.image;
    str(c->image.device, sizeof c->image.device, DEV10);
    c->image.size = 3500000u;
    memset(c->image.sha256, 0xC4, sizeof c->image.sha256);
    c->used_ms = NOW;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT32(3500000u, sent(1u)->size);
    TEST_ASSERT_EQUAL_MEMORY(c->image.sha256, sent(1u)->sha256, PLINK_SHA256_LEN);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FWK_PATH_RELAY_COPY, tr(1u)->path);
    TEST_ASSERT_FALSE(S.copy_free);
    says(1u, PLINK_FWS_RECEIVING, 0u);
    NOW += 60000u;
    c->used_ms = NOW;
    ready(1u);
    step();
    /* Keypad 1 is in: the copy goes, then keypad 2's is asked for. */
    TEST_ASSERT_TRUE(S.copy_free);
    memset(c, 0, sizeof *c);
    step();
    TEST_ASSERT_EQUAL_UINT8(2u, S.copy_for);
    /* Its facts refused by the server: blocked, never relayed. */
    c->state = PLINK_FWCOPY_FAILED;
    c->why = PLINK_FW_WHY_HELD_BACK;
    str(c->device, sizeof c->device, DEVICE);
    str(c->chip_rev, sizeof c->chip_rev, "v3");
    str(c->project, sizeof c->project, PROJECT);
    step();
    expect(2u, PLINK_FWK_BLOCKED, PLINK_FW_WHY_HELD_BACK);
    TEST_ASSERT_TRUE(S.copy_free);
    /* A copy that couldn't be fetched leaves no image, until the master's check is answered. */
    setUp();
    keypad(2u, OLD)->info.can = PLINK_FWI_RELAY;
    v3(2u);
    step();
    TEST_ASSERT_EQUAL_UINT8(2u, S.copy_for);
    c->state = PLINK_FWCOPY_FAILED;
    c->why = PLINK_FW_WHY_NO_CONNECT;
    str(c->device, sizeof c->device, DEVICE);
    str(c->chip_rev, sizeof c->chip_rev, "v3");
    str(c->project, sizeof c->project, PROJECT);
    step();
    expect(2u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_NO_IMAGE);
    TEST_ASSERT_TRUE(S.copy_free);
    memset(c, 0, sizeof *c);
    NOW += HOUR_MS;
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, S.copy_for);
    W.check_ms = NOW;
    step();
    TEST_ASSERT_EQUAL_UINT8(2u, S.copy_for);
    /* A ready copy is kept while a keypad waits for the relay's turn, and freed after 30 min
     * unused all the same. */
    setUp();
    keypad(2u, OLD)->info.can = PLINK_FWI_RELAY;
    keypad(3u, OLD)->info.can = PLINK_FWI_RELAY;
    str(kp(3u)->info.device, sizeof kp(3u)->info.device, DEV10);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(2u)->op);
    TEST_ASSERT_EQUAL_UINT8(3u, S.copy_for);
    says(2u, PLINK_FWS_RECEIVING, 0u);
    c->state = PLINK_FWCOPY_READY;
    str(c->device, sizeof c->device, DEV10);
    str(c->chip_rev, sizeof c->chip_rev, "v1");
    str(c->project, sizeof c->project, PROJECT);
    c->image = W.run.image;
    c->used_ms = NOW;
    step();
    TEST_ASSERT_FALSE(S.copy_free);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(3u)->op);
    expect(3u, PLINK_FWK_RELAY_WAIT, 0u);
    NOW += PLINK_FWP_COPY_IDLE_MS;
    says(2u, PLINK_FWS_RECEIVING, 0u);
    step();
    TEST_ASSERT_TRUE(S.copy_free);
}

/* INFO goes to a keypad with FW whose FW_INFO hasn't come, again 10 s after an answer that brought
 * none; nothing is judged nor cancelled before it. INFO names the version the master runs
 * (LINK-029: the one a keypad may not pass) once it is confirmed, and none while the master is on
 * its self-test. */
static void test_info_before_anything(void)
{
    keypad(1u, OLD);
    kp(1u)->have_info = kp(1u)->info_now = false;
    says(1u, PLINK_FWS_READY, 0u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(1u)->op);
    TEST_ASSERT_EQUAL_STRING(V, sent(1u)->version); /* the version it may not pass */
    expect(1u, PLINK_FWK_UNKNOWN, 0u);
    says(1u, PLINK_FWS_READY, 0u); /* answered; its FW_INFO lost */
    NOW += PLINK_FWP_INFO_MS - 1u;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    NOW += 1u;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(1u)->op);
    kp(1u)->have_info = true;
    step();
    expect(1u, PLINK_FWK_READY, 0u); /* its run, taken up */
    /* On its self-test the master names no version: it may yet go back. */
    setUp();
    W.own = PLINK_FWM_SELF_TEST;
    keypad(1u, OLD);
    kp(1u)->have_info = kp(1u)->info_now = false;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(1u)->op);
    TEST_ASSERT_EQUAL_STRING("", sent(1u)->version);
}

/* LINK-029: INFO names the master's version - the one a keypad may not pass - in each session of
 * each keypad with FW, whatever its state (on the target, newer, blocked), and again when the
 * confirmed version changes; an order of that session naming it does as well. Once a session: an
 * INFO still unanswered is the core's to resend. None on the master's self-test. It goes to a
 * relaying keypad - its RELAY stays the track's order, the core serves the relay across it - but
 * never to one told to install (its INSTALL goes again in a new session) nor one probing. */
static void test_info_names_the_version_each_session(void)
{
    keypad(1u, V);
    keypad(2u, NEXT);
    keypad(3u, OLD);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(3u)->op);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(1u)->op); /* told in this session already */
    TEST_ASSERT_EQUAL_UINT8(0u, sent(2u)->op);
    says(3u, PLINK_FWS_PROBING, 0u);
    NOW += 1500u;
    answer_probe(3u, PLINK_FW_WHY_HELD_BACK, "");
    step();
    expect(3u, PLINK_FWK_BLOCKED, PLINK_FW_WHY_HELD_BACK);
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    for (uint8_t d = 1u; d <= 3u; d++) {
        kp(d)->sessions++;
        kp(d)->state_now = false;
    }
    NOW += 1000u;
    step();
    for (uint8_t d = 1u; d <= 3u; d++) {
        TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(d)->op);
        TEST_ASSERT_EQUAL_STRING(V, sent(d)->version);
    }
    NOW += 20000u;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders()); /* unanswered: the core resends them */
    for (uint8_t d = 1u; d <= 3u; d++) {
        says(d, PLINK_FWS_IDLE, 0u);
    }
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    /* The master on its self-test of NEXT names nothing; confirmed, keypad 2 (on NEXT) is told
     * it, and the others are probed (their PROBE names it). */
    str(W.run.image.version, sizeof W.run.image.version, NEXT);
    W.run.image.sha256[0] ^= 0x5Au;
    W.own = PLINK_FWM_SELF_TEST;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    W.own = PLINK_FWM_IDLE;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(2u)->op);
    TEST_ASSERT_EQUAL_STRING(NEXT, sent(2u)->version);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
    TEST_ASSERT_EQUAL_STRING(NEXT, sent(1u)->version);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(3u)->op);
    /* New sessions mid-run: a relay is told (its RELAY kept), one probing isn't, one told to
     * install gets its INSTALL again. */
    setUp();
    keypad(1u, OLD)->info.can = PLINK_FWI_RELAY;
    keypad(2u, OLD);
    keypad(3u, OLD);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    says(1u, PLINK_FWS_RECEIVING, 0u);
    says(2u, PLINK_FWS_PROBING, 0u);
    says(3u, PLINK_FWS_PROBING, 0u);
    NOW += 1500u;
    answer_probe(3u, PLINK_FW_WHY_OK, V);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_FETCH, sent(3u)->op);
    ready(3u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(3u)->op);
    says(3u, PLINK_FWS_READY, 0u);
    NOW += 2000u;
    kp(1u)->sessions++;
    says(1u, PLINK_FWS_RECEIVING, 0u);
    kp(2u)->sessions++;
    says(2u, PLINK_FWS_PROBING, 0u);
    kp(3u)->sessions++;
    says(3u, PLINK_FWS_READY, 0u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(1u)->op);
    TEST_ASSERT_EQUAL_STRING(V, sent(1u)->version);
    expect(1u, PLINK_FWK_RELAY, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, tr(1u)->op);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(2u)->op);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(3u)->op);
    says(1u, PLINK_FWS_RECEIVING, 0u); /* the INFO's answer: its relay goes on, nothing resent */
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, sent(1u)->op);
    expect(1u, PLINK_FWK_RELAY, 0u);
}

/* LINK-030: UPDATED only on the keypad's word. Back on the target with probation over but nothing
 * more said (its core spoke before its service): still on its self-test; then back on its old
 * version - the canary, never installed again by itself. A keypad UPDATED that turns up behind the
 * target in a new session went back too, whatever its last section says (a cable, say); one on
 * the target by itself that turns up behind, saying it rolled back from it, holds its model. */
static void test_updated_only_on_the_keypads_word(void)
{
    keypad(1u, OLD);
    keypad(2u, OLD);
    all_fetching(2u);
    ready(1u);
    ready(2u);
    step();
    installs(1u);
    back(1u, V, true, 0u, PLINK_FW_LAST_NONE);
    step();
    expect(1u, PLINK_FWK_CHECKING, 0u);
    TEST_ASSERT_FALSE(starts(2u));
    back(1u, OLD, true, 0u, PLINK_FW_LAST_NONE); /* its service never said a word */
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_ROLLED_BACK);
    expect(2u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
    for (unsigned h = 0; h < 72u; h++) {
        NOW += HOUR_MS;
        W.check_ms = NOW;
        step();
        TEST_ASSERT_FALSE(starts(1u));
        TEST_ASSERT_FALSE(starts(2u));
    }
    /* UPDATED, then behind the target again in a new session. */
    setUp();
    keypad(1u, OLD);
    keypad(2u, OLD);
    probed(1u, PLINK_FW_WHY_OK, V);
    ready(1u);
    step();
    installs(1u);
    back(1u, V, true, 0u, PLINK_FW_LAST_UPDATED);
    step();
    expect(1u, PLINK_FWK_UPDATED, 0u);
    NOW += HOUR_MS;
    back(1u, OLD, true, 0u, PLINK_FW_LAST_NONE);
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_ROLLED_BACK);
    TEST_ASSERT_EQUAL_STRING(DEVICE, S.holds.canary[0].device);
    TEST_ASSERT_FALSE(starts(1u));
    expect(2u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
    /* On the target by itself, then behind it, saying it rolled back from it. */
    setUp();
    keypad(1u, V);
    step();
    expect(1u, PLINK_FWK_CURRENT, 0u);
    back(1u, OLD, true, 0u, PLINK_FW_LAST_ROLLED_BACK);
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_ROLLED_BACK);
    TEST_ASSERT_FALSE(starts(1u));
}

/* A keypad installing when the master's own update starts is followed, not cancelled; one that
 * goes back meanwhile holds its model - the master still runs V, so the hold is made - and when
 * the master's own update fails and it stays on V, no other keypad of that model installs V. */
static void test_a_rollback_while_the_master_updates_itself(void)
{
    keypad(1u, OLD);
    keypad(2u, OLD);
    all_fetching(2u);
    ready(1u);
    ready(2u);
    step();
    installs(1u);
    W.own = PLINK_FWM_UPDATING;
    step();
    expect(1u, PLINK_FWK_INSTALLING, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sent(2u)->op);
    says(2u, PLINK_FWS_STOPPED, PLINK_FW_WHY_CANCELLED);
    back(1u, OLD, true, 0u, PLINK_FW_LAST_ROLLED_BACK);
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_ROLLED_BACK);
    TEST_ASSERT_TRUE(S.holds_changed);
    TEST_ASSERT_EQUAL_STRING(V, S.holds.version);
    TEST_ASSERT_EQUAL_STRING(DEVICE, S.holds.canary[0].device);
    W.own = PLINK_FWM_IDLE; /* its own update failed: on V still */
    for (unsigned n = 0; n < 30u; n++) {
        NOW += MIN_MS;
        step();
        TEST_ASSERT_FALSE(starts(2u));
    }
    expect(2u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
}

/* LINK-035: a READY image the keypad discards after 24 h without its turn (armed all day) is no
 * failed run - nothing counted, the fall-back to the relay not used up - and it is fetched again
 * once the master is quiet. */
static void test_a_ready_image_kept_24_h_isnt_a_failure(void)
{
    keypad(1u, OLD);
    probed(1u, PLINK_FW_WHY_OK, V);
    ready(1u);
    W.quiet = false;
    step();
    expect(1u, PLINK_FWK_READY, 0u);
    NOW += DAY_MS;
    says(1u, PLINK_FWS_FAILED, PLINK_FW_WHY_TIMEOUT);
    step();
    expect(1u, PLINK_FWK_BEHIND, 0u);
    TEST_ASSERT_FALSE(tr(1u)->fell_back);
    for (size_t k = 0; k < PLINK_FWP_BUDGET; k++) {
        TEST_ASSERT_EQUAL_UINT64(0u, tr(1u)->fails[k]);
    }
    TEST_ASSERT_FALSE(starts(1u));
    W.quiet = true;
    probed(1u, PLINK_FW_WHY_OK, V);
    expect(1u, PLINK_FWK_FETCH, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_FETCH, sent(1u)->op);
}

/* OTA-027 across a master restart: a stop is kept as the time it ends, so one made 23 h before the
 * restart ends 1 h after it, whatever the new clock's start (the service hands in now plus what is
 * left, link/fwgo). */
static void test_a_stop_made_before_a_restart(void)
{
    keypad(1u, OLD);
    plink_fw_stop(&W.holds, 1u, V, NOW);
    TEST_ASSERT_EQUAL_UINT64(NOW + PLINK_FWP_HOLD_MS, W.holds.stop_until_ms[0]);
    step();
    expect(1u, PLINK_FWK_STOPPED, PLINK_FW_WHY_CANCELLED);
    master_restart();
    NOW = 1000u; /* its new clock */
    W.start_ms = W.check_ms = NOW;
    W.holds.stop_until_ms[0] = NOW + HOUR_MS; /* 23 h of the 24 gone */
    NOW += PLINK_FWP_SETTLE_MS;
    step();
    expect(1u, PLINK_FWK_STOPPED, PLINK_FW_WHY_CANCELLED);
    NOW = 1000u + HOUR_MS - 1u;
    step();
    TEST_ASSERT_FALSE(starts(1u));
    NOW += 1u;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT64(0u, S.holds.stop_until_ms[0]);
}

/* LINK-034, OTA-028: a CANCEL refused BUSY - its keypad's restart had started - leaves the keypad
 * holding its INSTALL. When the restart is then put off (READY again: arming, or a stop, in its
 * last seconds) the CANCEL goes again, so it never restarts later on an INSTALL the master gave up
 * (a person's Stop at the master here). Until that CANCEL is echoed the turn stays its own: no
 * other keypad is told to install. */
static void test_a_cancel_refused_busy_goes_again(void)
{
    keypad(1u, OLD);
    keypad(2u, OLD);
    all_fetching(2u);
    ready(1u);
    ready(2u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op);
    says(1u, PLINK_FWS_READY, 0u); /* echoed: its gate asks twice */
    step();
    plink_fw_stop(&W.holds, 1u, V, NOW); /* Stop updating keypad 1 */
    step();
    expect(1u, PLINK_FWK_STOPPED, PLINK_FW_WHY_CANCELLED);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(2u)->op);         /* not while that CANCEL is on its way */
    says(1u, PLINK_FWS_INSTALLING, PLINK_FW_WHY_BUSY); /* refused: its restart had started */
    NOW += 1000u;
    kp(2u)->state_ms = NOW;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    says(1u, PLINK_FWS_READY, PLINK_FW_WHY_NOT_QUIET); /* put off: READY, its INSTALL still held */
    NOW += 3000u;
    kp(2u)->state_ms = NOW;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sent(1u)->op); /* again */
    TEST_ASSERT_EQUAL_UINT(1u, orders());
    NOW += 1000u;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders()); /* once: the core resends it */
    says(1u, PLINK_FWS_STOPPED, PLINK_FW_WHY_CANCELLED);
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(2u)->op); /* the turn goes on */
}

/* LINK-030, LINK-031: a keypad whose FW_INFO the master doesn't have - a relay of its image taken
 * up blind after a master restart, READY now, its FW_INFO lost again and again - is never told to
 * install: no hold has judged it. Its facts are asked for (INFO); once they come, its model's
 * canary (kept in link/fwgo) holds it and its READY image is cancelled. A stop holds a keypad
 * whatever its facts: a relay taken up blind is cancelled at once. */
static void test_no_install_before_the_facts(void)
{
    str(W.holds.version, sizeof W.holds.version, V);
    str(W.holds.canary[0].device, sizeof W.holds.canary[0].device, DEVICE);
    str(W.holds.canary[0].chip_rev, sizeof W.holds.canary[0].chip_rev, "v1");
    W.start_ms = NOW;
    keypad(1u, OLD);
    kp(1u)->have_info = kp(1u)->info_now = false;
    plink_fw_state_t *s = says(1u, PLINK_FWS_RECEIVING, 0u);
    s->op = PLINK_FW_OP_RELAY;
    s->size = W.run.image.size;
    memcpy(s->id8, W.run.image.sha256, PLINK_ID8_LEN);
    step();
    expect(1u, PLINK_FWK_RELAY, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    says(1u, PLINK_FWS_RECEIVING, 0u);
    NOW += PLINK_FWP_SETTLE_MS;
    ready(1u);
    step();
    expect(1u, PLINK_FWK_READY, 0u);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(1u)->op);
    for (unsigned n = 0; n < 30u; n++) {
        says(1u, PLINK_FWS_READY, 0u); /* answered; its FW_INFO lost */
        NOW += 1000u;
        step();
        TEST_ASSERT_TRUE(sent(1u)->op != PLINK_FW_OP_INSTALL);
        TEST_ASSERT_FALSE(tr(1u)->installed);
    }
    kp(1u)->have_info = kp(1u)->info_now = true;
    says(1u, PLINK_FWS_READY, 0u);
    step();
    expect(1u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sent(1u)->op);
    /* A stop, before its facts. */
    setUp();
    W.start_ms = NOW;
    keypad(1u, OLD);
    kp(1u)->have_info = kp(1u)->info_now = false;
    s = says(1u, PLINK_FWS_RECEIVING, 0u);
    s->op = PLINK_FW_OP_RELAY;
    s->size = W.run.image.size;
    memcpy(s->id8, W.run.image.sha256, PLINK_ID8_LEN);
    step();
    expect(1u, PLINK_FWK_RELAY, 0u);
    says(1u, PLINK_FWS_RECEIVING, 0u);
    NOW += PLINK_FWP_SETTLE_MS;
    plink_fw_stop(&W.holds, 1u, V, NOW);
    step();
    expect(1u, PLINK_FWK_STOPPED, PLINK_FW_WHY_CANCELLED);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sent(1u)->op);
}

/* LINK-030: keypad 1's NO_RETURN made its model's hold; keypad 2, of the same model, then said in
 * its last section that it went back from the target. Keypad 1 heard again, still READY, never
 * restarted: its NO_RETURN is undone, but the hold stays - keypad 2's rollback holds the model -
 * so keypad 1 is held too and its READY image cancelled, never installed. */
static void test_an_undone_no_return_leaves_another_rollback(void)
{
    keypad(1u, OLD);
    keypad(2u, OLD);
    all_fetching(2u);
    ready(1u);
    ready(2u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op);
    says(1u, PLINK_FWS_READY, 0u);
    step();
    kp(1u)->online = false;
    kp(1u)->away = true;
    NOW += 31000u;
    step();
    expect(1u, PLINK_FWK_INSTALLING, 0u);
    NOW += PLINK_FWP_RETURN_MS;
    kp(2u)->state_ms = NOW;
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_NO_RETURN);
    expect(2u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
    says(2u, PLINK_FWS_STOPPED, PLINK_FW_WHY_CANCELLED);
    step();
    /* Keypad 2 in a new session: its last section says it went back from V. */
    back(2u, OLD, true, 0u, PLINK_FW_LAST_ROLLED_BACK);
    step();
    expect(2u, PLINK_FWK_FAILED, PLINK_FWK_WHY_ROLLED_BACK);
    TEST_ASSERT_EQUAL_STRING(DEVICE, S.holds.canary[0].device);
    /* Keypad 1 heard again in its session, still READY (its INFO echoed). */
    NOW += MIN_MS;
    kp(1u)->online = true;
    kp(1u)->away = false;
    says(1u, PLINK_FWS_READY, 0u);
    step();
    TEST_ASSERT_EQUAL_STRING(DEVICE, S.holds.canary[0].device);
    expect(1u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sent(1u)->op);
    NOW += HOUR_MS;
    says(1u, PLINK_FWS_STOPPED, PLINK_FW_WHY_CANCELLED);
    step();
    TEST_ASSERT_FALSE(starts(1u));
    TEST_ASSERT_FALSE(starts(2u));
}

/* LINK-030: one relay receives at a time - also when a master restart finds two keypads receiving
 * the image it holds (one's key accepted just before the restart, say): the lower is sent its RELAY
 * again, the other waits for the turn again, its run cancelled once the minute is over. */
static void test_two_relays_taken_up_at_once(void)
{
    W.start_ms = NOW;
    for (uint8_t d = 1u; d <= 2u; d++) {
        keypad(d, OLD);
        plink_fw_state_t *s = says(d, PLINK_FWS_RECEIVING, 0u);
        s->op = PLINK_FW_OP_RELAY;
        s->size = W.run.image.size;
        memcpy(s->id8, W.run.image.sha256, PLINK_ID8_LEN);
    }
    step();
    TEST_ASSERT_EQUAL_UINT(1u, orders());
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op);
    expect(1u, PLINK_FWK_RELAY, 0u);
    expect(2u, PLINK_FWK_RELAY_WAIT, 0u);
    says(1u, PLINK_FWS_RECEIVING, 0u);
    NOW += PLINK_FWP_SETTLE_MS;
    kp(1u)->state_ms = kp(2u)->state_ms = NOW;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sent(2u)->op);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(1u)->op);
    expect(2u, PLINK_FWK_RELAY_WAIT, 0u);
    says(2u, PLINK_FWS_STOPPED, PLINK_FW_WHY_CANCELLED);
    ready(1u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(2u)->op); /* its turn now */
}

/* LINK-031: the one copy is for a keypad that can take its relay now: one unheard while it waits
 * for the relay neither takes the copy nor keeps one alive - another keypad's is fetched
 * meanwhile. A copy for a keypad's facts that is larger than its slot blocks it (TOO_BIG) instead
 * of being fetched again and again. */
static void test_the_copy_is_for_a_keypad_heard(void)
{
    keypad(1u, OLD)->info.can = PLINK_FWI_RELAY;
    str(kp(1u)->info.device, sizeof kp(1u)->info.device, DEV10);
    keypad(2u, OLD)->info.can = PLINK_FWI_RELAY;
    v3(2u);
    step();
    expect(1u, PLINK_FWK_RELAY_WAIT, 0u);
    expect(2u, PLINK_FWK_RELAY_WAIT, 0u);
    TEST_ASSERT_EQUAL_UINT8(1u, S.copy_for);
    kp(1u)->online = false; /* unheard before its copy was asked for */
    kp(1u)->away = true;
    step();
    TEST_ASSERT_EQUAL_UINT8(2u, S.copy_for);
    /* A copy for keypad 1's facts, ready: freed while it is unheard - its turn goes on. */
    plink_fw_copy_t *c = &W.copy;
    c->state = PLINK_FWCOPY_READY;
    str(c->device, sizeof c->device, DEV10);
    str(c->chip_rev, sizeof c->chip_rev, "v1");
    str(c->project, sizeof c->project, PROJECT);
    c->image = W.run.image;
    str(c->image.device, sizeof c->image.device, DEV10);
    c->used_ms = NOW;
    step();
    TEST_ASSERT_TRUE(S.copy_free);
    memset(c, 0, sizeof *c);
    step();
    TEST_ASSERT_EQUAL_UINT8(2u, S.copy_for);
    /* Heard again (keypad 2 blocked meanwhile): its own is asked for. */
    c->state = PLINK_FWCOPY_FAILED;
    c->why = PLINK_FW_WHY_HELD_BACK;
    str(c->device, sizeof c->device, DEVICE);
    str(c->chip_rev, sizeof c->chip_rev, "v3");
    str(c->project, sizeof c->project, PROJECT);
    step();
    expect(2u, PLINK_FWK_BLOCKED, PLINK_FW_WHY_HELD_BACK);
    memset(c, 0, sizeof *c);
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, S.copy_for);
    kp(1u)->online = true;
    kp(1u)->away = false;
    step();
    TEST_ASSERT_EQUAL_UINT8(1u, S.copy_for);
    /* Larger than its slot. */
    setUp();
    keypad(1u, OLD)->info.can = PLINK_FWI_RELAY;
    str(kp(1u)->info.device, sizeof kp(1u)->info.device, DEV10);
    kp(1u)->info.slot = 3000000u;
    step();
    TEST_ASSERT_EQUAL_UINT8(1u, S.copy_for);
    c->state = PLINK_FWCOPY_READY;
    str(c->device, sizeof c->device, DEV10);
    str(c->chip_rev, sizeof c->chip_rev, "v1");
    str(c->project, sizeof c->project, PROJECT);
    c->image = W.run.image; /* 3.6 MB */
    str(c->image.device, sizeof c->image.device, DEV10);
    c->used_ms = NOW;
    step();
    expect(1u, PLINK_FWK_BLOCKED, PLINK_FW_WHY_TOO_BIG);
    TEST_ASSERT_TRUE(S.copy_free);
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    memset(c, 0, sizeof *c);
    for (unsigned h = 0; h < 3u; h++) {
        NOW += HOUR_MS;
        step();
        TEST_ASSERT_EQUAL_UINT8(0u, S.copy_for); /* not fetched again for nothing */
    }
}

/* LINK-030: a model's hold ends once a keypad of it whose passing is news to it - one that didn't
 * return in time and then did, or the model's Try again now - runs the target with its self-test
 * passed, whatever its track says; the end of its self-test, sent once, lost, is asked for (INFO).
 * Try again now never picks a keypad already on the target (it can't try it again: the model's
 * other keypads would stay held). The model's Try again now taken up after a master restart on its
 * self-test (CURRENT) ends the hold by its word too. */
static void test_a_return_on_the_target_ends_the_hold(void)
{
    keypad(1u, OLD);
    keypad(2u, OLD);
    all_fetching(2u);
    ready(1u);
    ready(2u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op);
    says(1u, PLINK_FWS_INSTALLING, 0u);
    step();
    kp(1u)->online = false;
    kp(1u)->away = true;
    NOW += PLINK_FWP_RETURN_MS;
    kp(2u)->state_ms = NOW;
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_NO_RETURN);
    expect(2u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
    says(2u, PLINK_FWS_STOPPED, PLINK_FW_WHY_CANCELLED);
    step();
    /* Keypad 1 turns up on V in a new session, on its self-test (the INFO of before echoed). */
    NOW += MIN_MS;
    back(1u, V, true, 1u, PLINK_FW_LAST_NONE);
    says(1u, PLINK_FWS_IDLE, 0u);
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_NO_RETURN);
    TEST_ASSERT_FALSE(starts(1u));
    says(1u, PLINK_FWS_IDLE, 0u); /* (its new session's INFO answered) */
    NOW += 30000u;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    /* Its self-test's end, sent once, is lost: a minute on, it is asked for. */
    NOW += PLINK_FWP_LATE_MS - 30000u;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INFO, sent(1u)->op);
    /* Try again now for every keypad before the answer: keypad 1 runs V already, so keypad 2 - the
     * model's other, behind - goes on, and keypad 1 is left to its word. */
    NOW += 1000u;
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        W.again_ms[i] = NOW;
    }
    step();
    TEST_ASSERT_EQUAL_UINT8(2u, S.holds.canary[0].retry);
    expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_NO_RETURN);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(2u)->op);
    /* The answer: its self-test of V passed - the model's hold ends. */
    plink_fw_state_t *s = says(1u, PLINK_FWS_IDLE, 0u);
    s->probation = 0u;
    s->last = PLINK_FW_LAST_UPDATED;
    step();
    expect(1u, PLINK_FWK_UPDATED, 0u);
    TEST_ASSERT_EQUAL_STRING("", S.holds.canary[0].device);
    /* The model's Try again now on its self-test as the master restarts (the hold in link/fwgo). */
    setUp();
    str(W.holds.version, sizeof W.holds.version, V);
    str(W.holds.canary[0].device, sizeof W.holds.canary[0].device, DEVICE);
    str(W.holds.canary[0].chip_rev, sizeof W.holds.canary[0].chip_rev, "v1");
    W.holds.canary[0].retry = 1u;
    W.start_ms = NOW;
    keypad(1u, V);
    keypad(2u, OLD);
    s = says(1u, PLINK_FWS_IDLE, 0u);
    s->probation = 1u;
    str(s->last_version, sizeof s->last_version, V);
    step();
    expect(1u, PLINK_FWK_CURRENT, 0u);
    expect(2u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
    s->probation = 0u;
    s->last = PLINK_FW_LAST_UPDATED;
    kp(1u)->state_ms = NOW;
    step();
    expect(1u, PLINK_FWK_UPDATED, 0u);
    TEST_ASSERT_EQUAL_STRING("", S.holds.canary[0].device);
    NOW += PLINK_FWP_SETTLE_MS;
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(2u)->op);
}

/* P2 decision 13, LINK-030: Try again now starts a held model with the keypad that went back when
 * it is heard. One that went back is often off (its power or network lost mid-install), and one
 * unheard is never ordered - the model's others would stay held - so the lowest heard keypad of
 * the model goes instead, the rest held until it is UPDATED. Pressed again while that one can't go
 * on (unheard before its INSTALL), the next heard one is picked. */
static void test_try_again_now_starts_with_a_keypad_heard(void)
{
    for (uint8_t d = 1u; d <= 3u; d++) {
        keypad(d, OLD);
    }
    all_fetching(3u);
    for (uint8_t d = 1u; d <= 3u; d++) {
        ready(d);
    }
    step();
    installs(1u);
    NOW += PLINK_FWP_RETURN_MS;
    kp(2u)->state_ms = kp(3u)->state_ms = NOW;
    step();
    expect(1u, PLINK_FWK_FAILED, PLINK_FWK_WHY_NO_RETURN);
    expect(2u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
    expect(3u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
    says(2u, PLINK_FWS_STOPPED, PLINK_FW_WHY_CANCELLED);
    says(3u, PLINK_FWS_STOPPED, PLINK_FW_WHY_CANCELLED);
    NOW += 1000u;
    W.again_ms[0] = NOW; /* plink fw update 1 */
    step();
    TEST_ASSERT_EQUAL_UINT8(2u, S.holds.canary[0].retry);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(2u)->op);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(3u)->op);
    expect(3u, PLINK_FWK_BLOCKED, PLINK_FWK_WHY_CANARY);
    /* Keypad 2 goes off before its INSTALL: pressed again, keypad 3 goes. */
    kp(2u)->online = false;
    kp(2u)->away = true;
    NOW += 1000u;
    W.again_ms[2] = NOW;
    step();
    TEST_ASSERT_EQUAL_UINT8(3u, S.holds.canary[0].retry);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(3u)->op);
    /* Pressed again while keypad 3 is heard: it stays the one. */
    says(3u, PLINK_FWS_PROBING, 0u);
    NOW += 1000u;
    W.again_ms[1] = NOW;
    step();
    TEST_ASSERT_EQUAL_UINT8(3u, S.holds.canary[0].retry);
}

/* LINK-030, LINK-034: an INSTALL taken whose keypad's gate goes on saying no while the master is
 * quiet (a kept tone, STATUSes that don't reach it: nothing the master sees end) holds the install
 * turn for at most 24 h of the master's quiet; time the master isn't quiet (armed at night) doesn't
 * count - its gate says no for the master's own reason then. Then a CANCEL voids that INSTALL and
 * discards the image; once it is echoed the turn goes on, and the keypad is fetched again,
 * nothing counted. */
static void test_an_install_at_its_gate_is_given_up_after_a_quiet_day(void)
{
    keypad(1u, OLD);
    keypad(2u, OLD);
    all_fetching(2u);
    ready(1u);
    ready(2u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op);
    says(1u, PLINK_FWS_READY, PLINK_FW_WHY_KEPT_TONE);
    step();
    for (unsigned h = 0; h < 31u; h++) {
        NOW += HOUR_MS;
        W.quiet = h < 10u || h >= 18u;             /* armed 8 h */
        kp(1u)->state_ms = kp(2u)->state_ms = NOW; /* FW_STATEs every 10 s */
        step();
        TEST_ASSERT_EQUAL_UINT(0u, orders());
        TEST_ASSERT_TRUE(tr(1u)->installed);
    }
    NOW += HOUR_MS;
    kp(1u)->state_ms = kp(2u)->state_ms = NOW;
    step(); /* 24 h of quiet */
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_CANCEL, sent(1u)->op);
    TEST_ASSERT_EQUAL_UINT8(0u, sent(2u)->op); /* not before that CANCEL is echoed */
    TEST_ASSERT_FALSE(tr(1u)->installed);
    NOW += 1000u;
    step();
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    says(1u, PLINK_FWS_STOPPED, PLINK_FW_WHY_CANCELLED);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(2u)->op);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op); /* fetched again */
    for (size_t k = 0; k < PLINK_FWP_BUDGET; k++) {
        TEST_ASSERT_EQUAL_UINT64(0u, tr(1u)->fails[k]);
    }
}

/* LINK-030: whether a keypad told to install restarted into the target is judged on that install
 * alone. One seen installing on an earlier try (its restart failed there: a flash fault) and
 * relayed again, then taken for installing as it went unheard after its INSTALL, back in a new
 * session on its old version with no run and its last section naming no rollback: it never tried
 * this one - probed again 10 min on, no canary. */
static void test_an_install_of_before_says_nothing_of_the_next(void)
{
    keypad(1u, OLD);
    probed(1u, PLINK_FW_WHY_OK, V);
    ready(1u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op);
    says(1u, PLINK_FWS_INSTALLING, 0u);
    step();
    expect(1u, PLINK_FWK_INSTALLING, 0u);
    TEST_ASSERT_TRUE(tr(1u)->tried);
    says(1u, PLINK_FWS_FAILED, PLINK_FW_WHY_FLASH); /* its restart failed: it never left */
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_RELAY, sent(1u)->op); /* the fetch falls back */
    says(1u, PLINK_FWS_RECEIVING, 0u);
    step();
    ready(1u);
    step();
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_INSTALL, sent(1u)->op);
    says(1u, PLINK_FWS_READY, 0u);
    step();
    kp(1u)->online = false;
    kp(1u)->away = true;
    NOW += 31000u;
    step();
    expect(1u, PLINK_FWK_INSTALLING, 0u);
    /* A power cut at its gate: a new session on its old version, no run, no rollback named. */
    back(1u, OLD, true, 0u, PLINK_FW_LAST_NONE);
    step();
    NOW += PLINK_FWP_RETURN_MS;
    kp(1u)->state_ms = NOW;
    step();
    TEST_ASSERT_TRUE(tr(1u)->state != PLINK_FWK_FAILED);
    TEST_ASSERT_EQUAL_STRING("", S.holds.canary[0].device);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FW_OP_PROBE, sent(1u)->op);
}

/* The planner reads only its world: nothing without one, and the same world gives the same step. */
static void test_pure(void)
{
    plink_fw_plan(NULL, NOW, &S);
    TEST_ASSERT_EQUAL_UINT(0u, orders());
    TEST_ASSERT_FALSE(S.holds_changed);
    plink_fw_plan(&W, NOW, NULL);
    plink_fw_stop(NULL, 0u, V, NOW);
    keypad(1u, OLD);
    keypad(2u, OLD)->info.can = PLINK_FWI_RELAY;
    plink_fw_step_t a, b;
    const plink_fw_world_t before = W;
    plink_fw_plan(&W, NOW, &a);
    plink_fw_plan(&W, NOW, &b);
    TEST_ASSERT_EQUAL_MEMORY(&before, &W, sizeof W);
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof a);
    /* The own slot's rules. */
    plink_fw_info_t info = kp(1u)->info;
    TEST_ASSERT_TRUE(plink_fw_own_fits(&W.run, &info));
    info.chip_rev_full = 200u;
    TEST_ASSERT_FALSE(plink_fw_own_fits(&W.run, &info));
    info.chip_rev_full = 0u;
    TEST_ASSERT_TRUE(plink_fw_own_fits(&W.run, &info));
    info.slot = W.run.image.size - 1u;
    TEST_ASSERT_FALSE(plink_fw_own_fits(&W.run, &info));
    info.slot = 0u; /* not said: any */
    TEST_ASSERT_TRUE(plink_fw_own_fits(&W.run, &info));
    W.run.have = false;
    TEST_ASSERT_FALSE(plink_fw_own_fits(&W.run, &info));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_path_table);
    RUN_TEST(test_unknown_version_waits_for_the_own_slot);
    RUN_TEST(test_probe_comes_first);
    RUN_TEST(test_a_late_refusal_ends_the_relay);
    RUN_TEST(test_orders_only_while_allowed);
    RUN_TEST(test_never_older);
    RUN_TEST(test_keypads_follow_the_master_by_themselves);
    RUN_TEST(test_probe_and_fetch_to_all_at_once);
    RUN_TEST(test_one_relay_at_a_time);
    RUN_TEST(test_one_install_at_a_time);
    RUN_TEST(test_install_waits_at_the_gate_and_away);
    RUN_TEST(test_a_rollback_holds_its_model_until_the_next_version);
    RUN_TEST(test_no_return_within_10_min);
    RUN_TEST(test_an_install_unheard_is_no_return_at_10_min);
    RUN_TEST(test_a_keypad_that_never_restarted_holds_nothing);
    RUN_TEST(test_a_keypad_back_from_away_is_asked_not_judged);
    RUN_TEST(test_a_restart_without_installing_holds_nothing);
    RUN_TEST(test_a_waiting_probe_is_asked_again);
    RUN_TEST(test_a_relay_at_key_wait_goes_on_after_a_master_restart);
    RUN_TEST(test_a_fetch_falls_back_to_the_relay);
    RUN_TEST(test_a_transfer_failure_is_tried_again);
    RUN_TEST(test_a_timeout_at_the_gate_isnt_counted);
    RUN_TEST(test_a_lost_fw_info_after_a_restart_is_asked_for);
    RUN_TEST(test_a_keypad_on_old_facts_is_asked_info_first);
    RUN_TEST(test_a_lost_end_of_run_is_asked_for);
    RUN_TEST(test_a_lost_run_is_tried_again);
    RUN_TEST(test_a_relay_in_a_new_session_is_asked_not_dropped);
    RUN_TEST(test_refusals_wait_for_new_facts);
    RUN_TEST(test_a_blocked_keypad_is_probed_again);
    RUN_TEST(test_a_stop_at_the_keypad_holds_24_h);
    RUN_TEST(test_a_stop_at_the_master_cancels);
    RUN_TEST(test_try_again_now);
    RUN_TEST(test_try_again_now_starts_with_the_canary);
    RUN_TEST(test_try_again_now_after_a_master_restart);
    RUN_TEST(test_holds_outlive_a_master_restart);
    RUN_TEST(test_no_keypad_restarts_in_a_loop);
    RUN_TEST(test_the_masters_own_update_cancels);
    RUN_TEST(test_runs_taken_up_after_a_master_restart);
    RUN_TEST(test_a_run_taken_up_from_a_probe_echo);
    RUN_TEST(test_the_minute_after_a_start);
    RUN_TEST(test_an_install_of_before_holds_nothing);
    RUN_TEST(test_a_relay_taken_up_before_its_facts);
    RUN_TEST(test_the_copy);
    RUN_TEST(test_info_before_anything);
    RUN_TEST(test_info_names_the_version_each_session);
    RUN_TEST(test_updated_only_on_the_keypads_word);
    RUN_TEST(test_a_rollback_while_the_master_updates_itself);
    RUN_TEST(test_a_ready_image_kept_24_h_isnt_a_failure);
    RUN_TEST(test_a_stop_made_before_a_restart);
    RUN_TEST(test_a_cancel_refused_busy_goes_again);
    RUN_TEST(test_no_install_before_the_facts);
    RUN_TEST(test_an_undone_no_return_leaves_another_rollback);
    RUN_TEST(test_two_relays_taken_up_at_once);
    RUN_TEST(test_the_copy_is_for_a_keypad_heard);
    RUN_TEST(test_a_return_on_the_target_ends_the_hold);
    RUN_TEST(test_try_again_now_starts_with_a_keypad_heard);
    RUN_TEST(test_an_install_at_its_gate_is_given_up_after_a_quiet_day);
    RUN_TEST(test_an_install_of_before_says_nothing_of_the_next);
    RUN_TEST(test_pure);
    return UNITY_END();
}
