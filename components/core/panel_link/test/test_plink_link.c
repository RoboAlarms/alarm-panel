/*
 * test_plink_link.c - the panel link end to end: the real master (plink_master.c) and up to eight
 * real keypads (plink_keypad.c) over the pretend network (plink_test_net.c), with the real engine
 * (alarm_sim) behind the master, wired as the platform wires it (docs/features/11 "Design",
 * "Roles on a P4 panel"):
 *   - every engine change is followed by its events and sounds, then a new snapshot (and one at
 *     least every second, as the alarm task's own tick gives);
 *   - port.run executes a keypad's command on the engine as {ALARM_SRC_KEYPAD, n}, answering from
 *     inside the call or after a delay (a test picks; the long run draws it);
 *   - port.lockout_left is alarm_engine_lockout_left, port.events_after reads the engine's
 *     recorded events, port.reachable and port.checkin go to the engine;
 *   - the keypads' configuration is plink_cfg_for_keypads of the engine's.
 * Every side runs on its own clock, as panels do: the master's is the engine's, each keypad's
 * counts from its own start, and a restart starts a clock again. The network's fake clock drives
 * them all.
 *
 * There are no known-answer vectors here: every byte on the wire comes from the codecs, pairing
 * and sessions, whose own suites prove them against the RFC vectors and the Python scripts beside
 * them (vectors_plink_*.py). This suite proves the two managers together: pairing from scratch to
 * a keypad that shows the master as it is; commands with codes, the entry countdown that never
 * goes up, the lockout of one keypad only, another partition refused, a keypad moved to another
 * home partition and the sounds that follow it; a lost event push filled in, and events pushed
 * ahead of their snapshot kept across a new session; a change on every
 * keypad within a second, with loss and jitter too; a master restart, and a keypad restart with
 * commands run once across it; one direction cut, a keypad switched off, the master's Wi-Fi down;
 * panic through an outage, run once; removal and pairing again; eight keypads pulling the
 * configuration through loss; replays after either side restarts; addresses moving; a hostile
 * keypad with valid keys flooding the master; six hours on a damaged network. After every test:
 * no AES-CCM key and nonce used twice on any side, no command run twice, nothing sent from inside
 * port.run, never two commands of one keypad running at once, no keypad ONLINE without a status
 * and a configuration.
 *
 * Every random number here (the network's faults, the crypto ports, the worker's delays) comes
 * from seeds; PLINK_LINK_SEED=<n> in the environment runs the suite on other ones.
 *
 * A keypad's voice (LINK-028) is the real policy (annunciator) behind plink_vfeed, fed from what
 * the keypad's manager holds as plink_keypad_svc.c feeds it; the master's is the same policy fed
 * straight from the engine as panel_app wires it (its own sounds tracked, its outputs, a commit
 * after every engine call). Both play every tone for 200 ms and every utterance at once, and the
 * kinds each decides are compared: the same ones on a working network; on a damaged one never a
 * kind the master did not decide about then, never one twice; after a keypad restart, a master
 * restart, a cut link and its backfill, nothing from before.
 *
 * Requirements: LINK-001, LINK-002, LINK-003, LINK-004, LINK-006, LINK-007, LINK-008, LINK-014,
 * LINK-015, LINK-016, LINK-018, LINK-019, LINK-021, LINK-023, LINK-024, LINK-026, LINK-027,
 * LINK-028 (and the keypad rules they serve: KPD-008, UI-023, D05 of docs/features/08).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm/alarm_codec.h"
#include "alarm/alarm_snapshot.h"
#include "annunciator/annunciator.h"
#include "panel_link/plink_keypad.h"
#include "panel_link/plink_master.h"
#include "panel_link/plink_vfeed.h"
#include "plink_test_net.h"
#include "plink_test_port.h"
#include "sim.h"
#include "unity.h"
#include "voice_pack/voice_pack.h"

#define KP_MAX        8u
#define EV_LOG        8192u
#define RAN_MAX       2048u
#define PEND_MAX      64u
#define REACH_MAX     512u
#define TYPES         0x40u
#define PUMP_EVERY_MS 1000u /* the alarm task's snapshots when nothing changed */
/* A countdown on a keypad against the master's: the network's delay and UI-023's hold of a
 * reading that rose by less than a second. */
#define SLACK_MS      1500u
#define EPOCH_BASE    1790000000000ll
#define ONE_MINUTE_S  60u /* LINK-007: the shortest supervision time */

enum { RUN_NOW, RUN_LATER, RUN_MIX };

static const uint8_t IP_M[4] = {192u, 0u, 2u, 1u};
static const uint8_t IP_X[4] = {198u, 51u, 100u, 66u};
static const plink_caps_t CAP = {ALARM_MAX_PARTITIONS, ALARM_MAX_ZONES, ALARM_MAX_USERS,
                                 ALARM_MAX_DEVICES};
static const char *const NAMES[KP_MAX] = {"Hall keypad",    "Kitchen keypad", "Garage keypad",
                                          "Bedroom keypad", "Office keypad",  "Porch keypad",
                                          "Cellar keypad",  "Attic keypad"};

/* ---- the rig --------------------------------------------------------------------------------- */

/* A voice: the real policy with a pretend pack (every clip 0.5 s), the zones' words from its
 * configuration, a pretend sounder (each one-shot tracked, PLAYED after V_TONE_MS) and the bell
 * test's 2 s; every utterance plays at once, and what it decided is logged. */
#define V_PACK_REV 3u
#define V_CLIP     8000u
#define V_TONE_MS  200u
#define V_BELL_MS  2000u /* panel_voice's PV_BELL_TEST_MS */
#define V_SAID_MAX 256u
#define V_TONES    32u

typedef struct {
    uint8_t kind;  /* ann_kind_t */
    uint16_t zone;
    uint64_t at;   /* the network's clock */
} said_t;

typedef struct {
    ann_t a;
    plink_vfeed_t f;          /* a keypad's */
    uint64_t now;             /* its own clock at the call under way */
    uint32_t ticket;
    struct {
        uint32_t ticket;
        uint64_t due;
    } tone[V_TONES];
    unsigned n_tone;
    uint64_t bell_due;        /* 0: no bell test pulse */
    unsigned baselines, pauses;
} vstate_t;

typedef struct {
    bool on;
    const plink_keypad_t *kk; /* a keypad's manager; NULL: the master's voice, the reference */
    vstate_t s;               /* started again with the panel */
    said_t said[V_SAID_MAX];  /* kept across restarts */
    unsigned n_said;
} voice_t;

/* A keypad panel: its manager, its crypto port, its NVS, and what its screen would show. */
typedef struct {
    int node;
    int64_t off; /* its clock = the network's + off (it counts from its own start) */
    unsigned boots;
    plink_test_port_t tp;
    plink_crypto_t c;
    plink_keypad_port_t port;
    plink_keypad_t k;
    uint8_t blob[ALARM_CONFIG_BLOB_MAX];
    alarm_config_t cfg;
    const char *name;
    uint16_t caps; /* the capability bits it offers at its next boot (0: none) */
    bool reused; /* an earlier boot's port saw a key and nonce twice */
    /* its NVS */
    bool have_rec;
    plink_keypad_rec_t rec;
    unsigned writes, erases;
    bool resolve_due;
    unsigned resolves;
    /* seen after every call into it */
    uint8_t state;
    char path[24]; /* the link states since its last boot: N(ot paired) C O(nline) L(ost) */
    size_t n_path;
    unsigned lost;
    unsigned lost_unexplained; /* LOST though a STATUS of those 30 s got through */
    unsigned lost_session;     /* LOST without the session the master holds */
    uint32_t sid;              /* its live session, as last seen */
    unsigned sessions;         /* new sessions it went to */
    uint64_t lost_at, online_at;
    uint64_t seen_at;
    uint64_t offline_ms; /* paired, switched on and not ONLINE */
    bool online_bad;     /* LINK-008: ONLINE without a status or a configuration */
    unsigned snd[32];    /* one-shot sounds it was told to play, by alarm_sound_t */
    unsigned got[TYPES]; /* frames delivered to it, by type */
    bool cd_have;        /* UI-023: the countdown its screen showed last */
    uint8_t cd_state;
    uint64_t cd_shown;
    unsigned cd_rises;
    voice_t v; /* LINK-028: its voice, when a test turns it on */
} kp_t;

/* A command port.run took, waiting for its delay. */
typedef struct {
    uint32_t ticket;
    uint8_t device;
    plink_cmd_t cmd;
    uint64_t due; /* the master's clock */
} pend_t;

/* A command the engine executed. */
typedef struct {
    uint8_t device;
    uint32_t req;
    uint8_t op, type, partition, result;
    uint8_t digest[PLINK_SHA256_LEN];
    uint64_t at; /* the network's clock */
} ran_t;

/* Someone holding a keypad's keys and session, sending from its own address. */
typedef struct {
    bool on;
    bool hellos; /* fresh HELLOs too (mac1 valid) */
    plink_session_t s;
    plink_keypad_rec_t rec;
    const plink_crypto_t *c;
    plink_hs_k_t hs;
    uint64_t next;
    uint32_t req, copy_req;
    unsigned bursts, cmds;
    unsigned got[TYPES]; /* frames the master sent it, by type */
} hostile_t;

typedef struct {
    plink_net_t net;
    sim_t sim;
    int64_t m_off; /* the master's clock (the engine's) = the network's + m_off */
    plink_test_port_t tpm;
    plink_crypto_t cm;
    bool m_reused;
    plink_master_port_t port;
    plink_master_t m;
    uint16_t m_caps; /* the capability bits the master offers at its next start (0: none) */
    unsigned m_got[TYPES]; /* frames delivered to the master, by type */
    int mnode, xnode;
    kp_t kp[KP_MAX];
    unsigned nkp;
    bool damaged; /* each port watches only its own seals (plink_test_port_t.only_dir) */
    /* the master's NVS and what its port saw */
    bool have_stored;
    plink_master_rec_t stored;
    unsigned n_store;
    uint32_t taken, held;
    struct {
        uint8_t device;
        bool up;
        uint64_t at;
    } reach[REACH_MAX];
    unsigned n_reach;
    unsigned checkins[PLINK_DEVICE_LAST + 1u];
    unsigned notes[16];
    /* commands */
    int run_mode;
    uint32_t run_delay_ms;
    uint64_t rng;
    pend_t pend[PEND_MAX];
    unsigned n_pend;
    ran_t ran[RAN_MAX];
    unsigned n_ran;
    bool in_run;
    unsigned sent_in_run;
    unsigned busy; /* two commands of one keypad (panic apart) running at once */
    /* the engine's records, kept past the sim's own */
    alarm_event_t ev[EV_LOG];
    unsigned n_ev, ev_fwd, snd_fwd, out_fwd;
    unsigned snd[ALARM_MAX_PARTITIONS + 1u][32]; /* one-shot sounds by partition and kind */
    alarm_snapshot_t snap;
    uint32_t snap_seq;
    bool pump_due;
    uint64_t pumped_at;
    bool snap_held; /* events and sounds are handed in, the snapshot waits (a store flush) */
    /* the keypads' configuration */
    alarm_config_t scratch;
    uint8_t blob[ALARM_CONFIG_BLOB_MAX];
    size_t blob_len;
    uint32_t blob_crc;
    alarm_config_t ref; /* the blob decoded here, apart from any keypad */
    bool watch;         /* UI-023: every keypad's countdown watched */
    bool explain;       /* every LOST checked against the network's log */
    hostile_t h;
    voice_t mv;         /* LINK-028: the master's voice, fed straight from the engine */
} rig_t;

static rig_t R;
static uint64_t SEED; /* PLINK_LINK_SEED, mixed into every seed (0: the usual ones) */
static uint8_t FBUF[PLINK_FRAME_MAX], PBUF[PLINK_FRAME_MAX];
static alarm_config_t CFG_TMP;

#define NOW (R.net.now_ms)

/* ---- clocks ---------------------------------------------------------------------------------- */

static uint64_t clock_of(uint64_t net, int64_t off)
{
    return (uint64_t)((int64_t)net + off);
}

static uint64_t net_of(uint64_t t, int64_t off)
{
    if (t == UINT64_MAX) {
        return UINT64_MAX;
    }
    const int64_t v = (int64_t)t - off;
    return v > 0 ? (uint64_t)v : 0u;
}

static uint64_t mnow(void)
{
    return clock_of(NOW, R.m_off);
}

static uint64_t know(const kp_t *k)
{
    return clock_of(NOW, k->off);
}

static int64_t epoch_now(void)
{
    return EPOCH_BASE + (int64_t)NOW;
}

static const plink_addr_t *maddr(void)
{
    return plink_net_addr(&R.net, R.mnode);
}

static uint8_t dev(const kp_t *k)
{
    return k->rec.device;
}

static uint64_t rnd(void)
{
    uint64_t z = (R.rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static uint64_t min64(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

static uint64_t left_now(uint64_t field, uint64_t taken, uint64_t now)
{
    const uint64_t gone = now > taken ? now - taken : 0u;
    return field > gone ? field - gone : 0u;
}

static bool near(uint64_t a, uint64_t b)
{
    return (a > b ? a - b : b - a) <= SLACK_MS;
}

/* ---- the voices (LINK-028) ------------------------------------------------------------------- */

static uint32_t v_clip(void *ctx, uint16_t id)
{
    (void)ctx;
    return id >= VP_ENDING_FIRST ? 0u : V_CLIP; /* no endings: every word as itself */
}

/* A zone's words from the voice's configuration: the engine's for the master, the keypad's copy
 * from the link for a keypad (panel_voice reads them the same way). */
static bool v_words(void *ctx, uint16_t zone, ann_zone_words_t *out)
{
    const voice_t *v = ctx;
    const alarm_config_t *c = v->kk != NULL ? plink_keypad_config(v->kk) : &R.sim.cfg;
    if (c == NULL || zone == 0u || zone > ALARM_MAX_ZONES) {
        return false;
    }
    const alarm_zone_config_t *zc = &c->zones[zone - 1u];
    memset(out, 0, sizeof *out);
    const unsigned n = alarm_zone_word_count(zc);
    for (unsigned i = 0; i < n && i < ANN_CLIPS_MAX; i++) {
        out->ids[i] = alarm_zone_word(zc, i);
    }
    out->count = (uint8_t)(n < ANN_CLIPS_MAX ? n : ANN_CLIPS_MAX);
    out->binding_revision = zc->voice_binding_revision;
    out->approved_revision = zc->voice_approved_revision;
    out->pack_revision = zc->voice_pack_revision;
    return out->count != 0u;
}

/* The voice as it starts with its panel: Announcements on (D06), a ready pack, no baseline yet. */
static void v_start(voice_t *v, const plink_keypad_t *kk, uint8_t part, uint64_t now)
{
    memset(&v->s, 0, sizeof v->s);
    v->on = true;
    v->kk = kk;
    v->s.now = now;
    const ann_ports_t ports = {v, v_words, v_clip};
    ann_init(&v->s.a, &ports, part);
    ann_join_exit(&v->s.a, kk != NULL); /* panel_voice with a source: a link keypad */
    const ann_prefs_t p = {true, true, true, 70u, false};
    ann_set_prefs(&v->s.a, &p, now);
    const ann_pack_t pack = {true, V_PACK_REV, VP_WORD_GAP_MS, VP_PART_GAP_MS};
    ann_set_pack(&v->s.a, &pack, now);
}

/* The pretend sounder: a one-shot tracked, its ticket PLAYED V_TONE_MS later. */
static uint32_t v_tone(voice_t *v, uint8_t sound)
{
    if (sound < (uint8_t)ALARM_SOUND_ACK || v->s.n_tone == V_TONES) {
        return 0u;
    }
    v->s.tone[v->s.n_tone].ticket = ++v->s.ticket;
    v->s.tone[v->s.n_tone].due = v->s.now + V_TONE_MS;
    v->s.n_tone++;
    return v->s.ticket;
}

/* Every utterance it may start now, played at once. */
static void v_drain(voice_t *v)
{
    ann_utt_t u;
    while (ann_next(&v->s.a, v->s.now, &u)) {
        if (v->n_said < V_SAID_MAX) {
            v->said[v->n_said].kind = u.kind;
            v->said[v->n_said].zone = u.zone;
            v->said[v->n_said].at = NOW;
            v->n_said++;
        }
        ann_done(&v->s.a, u.id, v->s.now);
    }
}

/* Tones that ended, the bell pulse's end, then what may be said. */
static void v_step(voice_t *v, uint64_t now)
{
    v->s.now = now;
    for (unsigned i = 0; i < v->s.n_tone;) {
        if (v->s.tone[i].due <= now) {
            const uint32_t t = v->s.tone[i].ticket;
            v->s.tone[i] = v->s.tone[--v->s.n_tone];
            ann_ticket(&v->s.a, t, true, now);
        } else {
            i++;
        }
    }
    if (v->s.bell_due != 0u && now >= v->s.bell_due) {
        v->s.bell_due = 0u;
        ann_bell_test_done(&v->s.a, now);
    }
    v_drain(v);
}

static uint64_t v_next(const voice_t *v)
{
    if (!v->on) {
        return UINT64_MAX;
    }
    uint64_t t = v->s.bell_due != 0u ? v->s.bell_due : UINT64_MAX;
    for (unsigned i = 0; i < v->s.n_tone; i++) {
        t = min64(t, v->s.tone[i].due);
    }
    return v->kk != NULL ? min64(t, plink_vfeed_next(&v->s.f)) : t;
}

static void v_output(voice_t *v, alarm_output_t o, uint8_t part, alarm_pattern_t pat)
{
    ann_output(&v->s.a, o, part, pat);
    if (o == ALARM_OUT_BELL && part == v->s.a.keypad && pat == ALARM_PAT_TEST) {
        v->s.bell_due = v->s.now + V_BELL_MS;
    }
}

/* The state after the batch, as panel_voice_engine_snapshot computes it. */
static void v_commit(voice_t *v, const alarm_snapshot_t *s)
{
    ann_state_t st;
    ann_state_from_snapshot(s, v->s.a.keypad, s->exit_sounds, &st);
    ann_commit(&v->s.a, &st, v->s.now);
    v_drain(v);
}

/* A keypad voice's port, doing what panel_voice does with each call. */
static void kv_baseline(void *ctx, uint8_t part, const alarm_snapshot_t *s)
{
    voice_t *v = ctx;
    ann_rebaseline(&v->s.a); /* the drop the baseline counts */
    v->s.bell_due = 0u;
    if (v->s.a.keypad != part) {
        ann_set_keypad(&v->s.a, part);
    }
    v_commit(v, s); /* silent */
    v->s.baselines++;
}

static void kv_pause(void *ctx)
{
    voice_t *v = ctx;
    ann_rebaseline(&v->s.a);
    v->s.bell_due = 0u;
    v->s.pauses++;
}

static void kv_event(void *ctx, const alarm_event_t *ev)
{
    ann_event(&((voice_t *)ctx)->s.a, ev);
}

static void kv_sound(void *ctx, uint8_t part, alarm_sound_t sound, uint16_t zone, uint32_t ticket)
{
    ann_sound(&((voice_t *)ctx)->s.a, part, sound, zone, ticket);
}

static void kv_output(void *ctx, alarm_output_t o, uint8_t part, alarm_pattern_t pat)
{
    v_output(ctx, o, part, pat);
}

static void kv_snapshot(void *ctx, const alarm_snapshot_t *s)
{
    v_commit(ctx, s);
}

/* The master's voice from now on, for partition `part`: its first commit is the baseline. */
static void ref_start(uint8_t part)
{
    v_start(&R.mv, NULL, part, clock_of(NOW, R.m_off));
    v_commit(&R.mv, &R.snap);
}

/* The engine's wall clock is the network's (one clock in the house): an event's stamp and the
 * master's time in its STATUS and TIME agree, as on panels whose clocks are set. */
static int64_t rig_wall_clock(void *ctx)
{
    (void)ctx;
    return epoch_now();
}

/* ---- the engine, as the platform drives it --------------------------------------------------- */

static void sync_engine(uint64_t t)
{
    if (t > R.sim.now) {
        R.sim.now = t;
    }
    alarm_engine_tick(&R.sim.eng, R.sim.now);
}

/* What the platform does after the engine changed: the events and sounds it made, then a new
 * snapshot (its seq rises every time; the master coalesces). */
static void pump(uint64_t t)
{
    voice_t *ref = R.mv.on ? &R.mv : NULL; /* LINK-028: the master's voice, as panel_app feeds it */
    if (ref != NULL) {
        ref->s.now = t;
    }
    while (R.ev_fwd < R.sim.n_events) {
        const alarm_event_t *e = &R.sim.events[R.ev_fwd++];
        if (R.n_ev > 0u && e->seq <= R.ev[R.n_ev - 1u].seq) {
            R.n_ev = 0u; /* the log started again */
        }
        TEST_ASSERT_TRUE_MESSAGE(R.n_ev < EV_LOG, "the rig's event log is full");
        R.ev[R.n_ev++] = *e;
        plink_master_event(&R.m, t, e);
        if (ref != NULL) {
            ann_event(&ref->s.a, e);
        }
    }
    while (R.snd_fwd < R.sim.n_sounds) {
        const sim_sound_t *s = &R.sim.sounds[R.snd_fwd++];
        if (s->partition <= ALARM_MAX_PARTITIONS && (unsigned)s->sound < 32u) {
            R.snd[s->partition][s->sound]++;
        }
        plink_master_sound(&R.m, t, s->partition, (uint8_t)s->sound, s->zone);
        if (ref != NULL) {
            /* panel_app's on_sound: its own partition's and partition 0's tones, tracked */
            const bool here = s->partition == 0u || s->partition == ref->s.a.keypad;
            const uint32_t ticket = here ? v_tone(ref, (uint8_t)s->sound) : 0u;
            ann_sound(&ref->s.a, s->partition, s->sound, s->zone, ticket);
        }
    }
    while (R.out_fwd < R.sim.n_outputs) {
        const sim_output_t *o = &R.sim.outputs[R.out_fwd++];
        if (ref != NULL) {
            v_output(ref, o->output, o->partition, o->pattern);
        }
    }
    /* The sim records 512 of each: start its records again once everything was handed in. */
    if (R.sim.n_events > SIM_MAX_RECORDS / 2u) {
        R.sim.n_events = 0u;
        R.ev_fwd = 0u;
    }
    if (R.sim.n_sounds > SIM_MAX_RECORDS / 2u) {
        R.sim.n_sounds = 0u;
        R.snd_fwd = 0u;
    }
    if (R.sim.n_outputs > SIM_MAX_RECORDS / 2u) {
        R.sim.n_outputs = 0u;
        R.out_fwd = 0u;
    }
    R.pump_due = false;
    R.pumped_at = t;
    if (R.snap_held) {
        return;
    }
    alarm_engine_snapshot(&R.sim.eng, &R.snap);
    R.snap.seq = ++R.snap_seq;
    plink_master_snapshot(&R.m, t, &R.snap, epoch_now(), PLINK_REPORT_ONLINE);
    if (ref != NULL) {
        v_commit(ref, &R.snap);
    }
}

/* A command on the engine as {ALARM_SRC_KEYPAD, device}, logged. */
static plink_result_t exec(uint8_t device, const plink_cmd_t *c)
{
    plink_result_t r;
    memset(&r, 0, sizeof r);
    r.req = c->req;
    const alarm_source_t src = {ALARM_SRC_KEYPAD, device};
    sync_engine(mnow());
    if (c->op == PLINK_OP_COMMAND) {
        alarm_cmd_result_t cr;
        memset(&cr, 0, sizeof cr);
        r.result = (uint8_t)alarm_engine_command(&R.sim.eng, &c->cmd, &src, R.sim.now, &cr);
        r.user = cr.user;
        r.not_ready_zone = cr.not_ready_zone;
        r.may_disarm = cr.may_disarm ? 1u : 0u;
    } else if (c->op == PLINK_OP_AUTH) {
        uint8_t user = 0u;
        r.result = (uint8_t)alarm_engine_authenticate(&R.sim.eng, &src, c->cmd.code,
                                                      c->cmd.partition, R.sim.now, &user);
        r.user = user;
    } else {
        r.result = (uint8_t)ALARM_ERR_UNAVAILABLE;
    }
    TEST_ASSERT_TRUE_MESSAGE(R.n_ran < RAN_MAX, "the rig's command log is full");
    ran_t *x = &R.ran[R.n_ran++];
    x->device = device;
    x->req = c->req;
    x->op = c->op;
    x->type = (uint8_t)c->cmd.type;
    x->partition = c->cmd.partition;
    x->result = r.result;
    x->at = NOW;
    plink_cmd_digest(c, x->digest);
    R.pump_due = true;
    return r;
}

static bool is_panic(const plink_cmd_t *c)
{
    return c->op == PLINK_OP_COMMAND && c->cmd.type == ALARM_CMD_PANIC;
}

/* The commands whose delay is up: run, the engine's change handed in, then the answer. */
static void runs_due(uint64_t t)
{
    for (unsigned i = 0; i < R.n_pend;) {
        if (R.pend[i].due > t) {
            i++;
            continue;
        }
        pend_t p = R.pend[i];
        R.pend[i] = R.pend[--R.n_pend];
        const plink_result_t res = exec(p.device, &p.cmd);
        plink_wipe(&p.cmd, sizeof p.cmd); /* the platform's copy of the codes */
        pump(t);
        plink_master_result(&R.m, t, p.ticket, &res);
    }
}

/* The master's own touchscreen ({KEYPAD, LOCAL}). */
static alarm_result_t master_cmd(alarm_cmd_type_t type, uint8_t part, alarm_arm_level_t level,
                                 const char *code)
{
    alarm_cmd_t c;
    memset(&c, 0, sizeof c);
    c.type = type;
    c.partition = part;
    c.level = level;
    snprintf(c.code, sizeof c.code, "%s", code);
    const alarm_source_t src = {ALARM_SRC_KEYPAD, ALARM_KEYPAD_LOCAL};
    sync_engine(mnow());
    const alarm_result_t r = alarm_engine_command(&R.sim.eng, &c, &src, R.sim.now, NULL);
    pump(mnow());
    return r;
}

/* A zone opens or closes at the master. */
static void m_zone(uint16_t zone, bool fault)
{
    sync_engine(mnow());
    if (fault) {
        sim_fault(&R.sim, zone);
    } else {
        sim_restore(&R.sim, zone);
    }
    pump(mnow());
}

/* ---- the master's port ----------------------------------------------------------------------- */

static bool p_send(void *ctx, const plink_addr_t *to, const uint8_t *frame, size_t len)
{
    (void)ctx;
    if (R.in_run) {
        R.sent_in_run++;
    }
    return plink_net_send(&R.net, R.mnode, to, frame, len);
}

static bool p_store(void *ctx, const plink_master_rec_t *rec)
{
    (void)ctx;
    R.stored = *rec;
    R.have_stored = true;
    R.n_store++;
    return true;
}

static bool p_run(void *ctx, uint32_t ticket, uint8_t device, const plink_cmd_t *cmd)
{
    (void)ctx;
    TEST_ASSERT_FALSE_MESSAGE(R.in_run, "port.run called from inside port.run");
    /* LINK-027: one command of a keypad at a time, panic in a slot of its own. */
    for (unsigned i = 0; i < R.n_pend; i++) {
        if (R.pend[i].device == device && is_panic(&R.pend[i].cmd) == is_panic(cmd)) {
            R.busy++;
        }
    }
    int mode = R.run_mode;
    uint32_t delay = R.run_delay_ms;
    if (mode == RUN_MIX) {
        mode = (rnd() & 1u) != 0u ? RUN_NOW : RUN_LATER;
        delay = (uint32_t)(rnd() % 600u);
    }
    if (mode == RUN_NOW) {
        /* The answer from inside port.run: the master holds it until run is back. */
        R.in_run = true;
        const plink_result_t res = exec(device, cmd);
        plink_master_result(&R.m, mnow(), ticket, &res);
        R.in_run = false;
        return true;
    }
    TEST_ASSERT_TRUE(R.n_pend < PEND_MAX);
    pend_t *p = &R.pend[R.n_pend++];
    p->ticket = ticket;
    p->device = device;
    p->cmd = *cmd;
    p->due = mnow() + delay;
    return true;
}

static void p_reachable(void *ctx, uint8_t device, bool up)
{
    (void)ctx;
    TEST_ASSERT_TRUE(R.n_reach < REACH_MAX);
    R.reach[R.n_reach].device = device;
    R.reach[R.n_reach].up = up;
    R.reach[R.n_reach].at = NOW;
    R.n_reach++;
    /* alarm_service_device_reachable: the engine's device supervision trouble (CORE-TRBL-008) */
    sync_engine(mnow());
    alarm_engine_device_reachable(&R.sim.eng, device, up, R.sim.now);
    R.pump_due = true;
}

static void p_checkin(void *ctx, uint8_t device)
{
    (void)ctx;
    if (device <= PLINK_DEVICE_LAST) {
        R.checkins[device]++;
    }
    /* alarm_service_device_checkin: the engine's check-in, which clears a device trouble */
    sync_engine(mnow());
    alarm_engine_device_checkin(&R.sim.eng, device, R.sim.now);
    R.pump_due = true;
}

static uint32_t p_taken(void *ctx)
{
    (void)ctx;
    return R.taken;
}

static void p_held(void *ctx, uint32_t held)
{
    (void)ctx;
    R.held = held;
}

static uint32_t p_lockout(void *ctx, uint8_t device, uint64_t now)
{
    (void)ctx;
    const alarm_source_t src = {ALARM_SRC_KEYPAD, device};
    return (uint32_t)alarm_engine_lockout_left(&R.sim.eng, &src, now);
}

/* The newest events after after_seq, oldest first, at most max. */
static size_t p_events_after(void *ctx, uint32_t after, alarm_event_t *out, size_t max, bool *gap)
{
    (void)ctx;
    size_t first = 0;
    while (first < R.n_ev && R.ev[first].seq <= after) {
        first++;
    }
    const size_t avail = R.n_ev - first;
    const size_t k = avail < max ? avail : max;
    *gap = k < avail;
    memcpy(out, &R.ev[R.n_ev - k], k * sizeof *out);
    return k;
}

static void p_note(void *ctx, plink_note_t n, uint8_t device)
{
    (void)ctx;
    (void)device;
    if ((unsigned)n < 16u) {
        R.notes[n]++;
    }
}

static unsigned edges(uint8_t device, bool up)
{
    unsigned c = 0;
    for (unsigned i = 0; i < R.n_reach; i++) {
        c += R.reach[i].device == device && R.reach[i].up == up;
    }
    return c;
}

static uint64_t edge_at(uint8_t device, bool up)
{
    for (unsigned i = 0; i < R.n_reach; i++) {
        if (R.reach[i].device == device && R.reach[i].up == up) {
            return R.reach[i].at;
        }
    }
    return UINT64_MAX;
}

/* Times the engine executed (device, req). */
static unsigned ran_req(uint8_t device, uint32_t req)
{
    unsigned c = 0;
    for (unsigned i = 0; i < R.n_ran; i++) {
        c += R.ran[i].device == device && R.ran[i].req == req;
    }
    return c;
}

static unsigned ran_dev(uint8_t device)
{
    unsigned c = 0;
    for (unsigned i = 0; i < R.n_ran; i++) {
        c += R.ran[i].device == device;
    }
    return c;
}

/* ---- the master as a network node ------------------------------------------------------------ */

static void m_input(void *ctx, uint64_t now, const plink_addr_t *from, const uint8_t *f, size_t len)
{
    (void)ctx;
    if (len > 3u && f[3] < TYPES) {
        R.m_got[f[3]]++;
    }
    plink_master_input(&R.m, clock_of(now, R.m_off), from, f, len);
}

static void m_tick(void *ctx, uint64_t now)
{
    (void)ctx;
    const uint64_t t = clock_of(now, R.m_off);
    const bool due = alarm_engine_next_deadline(&R.sim.eng) <= t;
    sync_engine(t);
    runs_due(t);
    if (due || R.pump_due || t >= R.pumped_at + PUMP_EVERY_MS) {
        pump(t);
    }
    plink_master_tick(&R.m, t);
    if (R.mv.on) {
        v_step(&R.mv, t);
    }
}

static uint64_t m_next(void *ctx)
{
    (void)ctx;
    uint64_t t = min64(plink_master_next(&R.m), alarm_engine_next_deadline(&R.sim.eng));
    t = min64(t, v_next(&R.mv));
    for (unsigned i = 0; i < R.n_pend; i++) {
        t = min64(t, R.pend[i].due);
    }
    t = min64(t, R.pump_due ? mnow() : R.pumped_at + PUMP_EVERY_MS);
    return net_of(t, R.m_off);
}

/* ---- the keypads ----------------------------------------------------------------------------- */

/* UI-023: the seconds a keypad's screen shows during a delay never go up within one delay. */
static void countdown(kp_t *k, uint64_t t, uint8_t home)
{
    const alarm_snapshot_t *s = plink_keypad_snapshot(&k->k);
    if (home < 1u || home > ALARM_MAX_PARTITIONS) {
        k->cd_have = false;
        return;
    }
    const alarm_partition_status_t *p = &s->part[home - 1u];
    if (p->state != ALARM_STATE_EXIT_DELAY && p->state != ALARM_STATE_ENTRY_DELAY) {
        k->cd_have = false;
        return;
    }
    const uint64_t shown = (left_now(p->delay_remaining_ms, s->taken_ms, t) + 999u) / 1000u;
    if (k->cd_have && k->cd_state == (uint8_t)p->state && shown > k->cd_shown) {
        k->cd_rises++;
    }
    k->cd_have = true;
    k->cd_state = (uint8_t)p->state;
    k->cd_shown = shown;
}

/* A keypad just went LOST on a damaged network: LINK-006 and LINK-008 say that may happen only
 * when every STATUS the master sent it since the one it last applied - one every 10 s, so at
 * least two in those 30 s, the network's jitter aside - was lost or damaged on the way (those
 * sent in the last 400 ms may still be under way: not counted); and the master still holds its
 * session, or its old one while a handshake hands over (a status lost is no session lost). */
static void lost_explained(kp_t *k)
{
    unsigned sent = 0, gone = 0;
    for (size_t e = 0; e < R.net.log_count; e++) {
        const plink_net_seen_t *s = &R.net.log[e];
        if (s->from == R.mnode && s->to == k->node && s->type == PLINK_T_STATUS &&
            s->at_ms + PLINK_LOST_MS >= NOW && s->at_ms + 400u <= NOW) {
            sent++;
            gone += s->lost || s->corrupted;
        }
    }
    if (sent < PLINK_LOST_MS / PLINK_STATUS_EVERY_MS - 1u || gone < sent) {
        k->lost_unexplained++;
    }
    /* (in a handshake the keypad's new session is tentative and the master's still the old one) */
    const plink_peer_t *p = &R.m.peer[dev(k) - 1u];
    const bool same = k->k.live.valid && p->live.valid && p->live.sid == k->k.live.sid;
    const bool handing_over =
        k->k.tentative && k->k.old.valid && p->live.valid && p->live.sid == k->k.old.sid;
    if (!same && !handing_over) {
        k->lost_session++;
    }
}

/* The network's log keeps the last 40 s (it holds 1024 frames). */
static void log_trim(void)
{
    size_t keep = 0;
    for (size_t e = 0; e < R.net.log_count; e++) {
        if (R.net.log[e].at_ms + 40000u >= NOW) {
            R.net.log[keep++] = R.net.log[e];
        }
    }
    R.net.log_count = keep;
}

/* What the keypad's UI reads after every call into it. */
static void observe(kp_t *k)
{
    plink_keypad_info_t in;
    const uint64_t t = know(k);
    plink_keypad_info(&k->k, t, &in);
    if (k->seen_at != 0u && k->state != PLINK_LINK_ONLINE && k->state != PLINK_LINK_NOT_PAIRED) {
        k->offline_ms += NOW - k->seen_at;
    }
    k->seen_at = NOW;
    if (k->k.live.valid && k->k.live.sid != k->sid) {
        k->sid = k->k.live.sid;
        k->sessions++;
    }
    if (in.state != k->state) {
        static const char LETTER[4] = {'N', 'C', 'O', 'L'};
        if (k->n_path + 1u < sizeof k->path && in.state < 4u) {
            k->path[k->n_path++] = LETTER[in.state];
        }
        if (in.state == PLINK_LINK_LOST) {
            k->lost++;
            k->lost_at = NOW;
            if (R.explain) {
                lost_explained(k);
            }
        }
        if (in.state == PLINK_LINK_ONLINE) {
            k->online_at = NOW;
        }
        k->state = in.state;
    }
    /* LINK-008: ONLINE only with a status and the configuration in. */
    if (in.state == PLINK_LINK_ONLINE &&
        (in.status_age_s == UINT32_MAX || plink_keypad_config(&k->k) == NULL)) {
        k->online_bad = true;
    }
    plink_sound_t s;
    while (plink_keypad_take_sound(&k->k, &s)) {
        k->snd[s.sound < 32u ? s.sound : 0u]++;
        if (k->v.on) {
            /* plink_keypad_svc.c sounds(): tracked while the voice is fed, then handed on */
            k->v.s.now = t;
            const uint32_t ticket = plink_vfeed_live(&k->v.s.f) ? v_tone(&k->v, s.sound) : 0u;
            plink_vfeed_sound(&k->v.s.f, t, &s, ticket);
        }
    }
    plink_time_t tm;
    (void)plink_keypad_take_time(&k->k, &tm);
    if (R.watch) {
        countdown(k, t, in.home);
    }
    if (k->v.on) {
        /* plink_keypad_svc.c voice_feed() */
        static alarm_event_t evs[PLINK_EVENTS_KEPT];
        const size_t n = plink_keypad_events(&k->k, evs, PLINK_EVENTS_KEPT);
        const plink_vfeed_view_t view = {in.state == PLINK_LINK_ONLINE,
                                         in.home,
                                         plink_keypad_snapshot(&k->k),
                                         evs,
                                         n,
                                         plink_keypad_epoch_ms(&k->k, t),
                                         false,
                                         plink_keypad_mono_ms(&k->k, t)};
        k->v.s.now = t;
        plink_vfeed_poll(&k->v.s.f, t, &view);
        v_step(&k->v, t);
    }
}

/* LINK-028: keypad k's voice from now on (as its panel starts it), fed through plink_vfeed. */
static void kv_start(kp_t *k)
{
    v_start(&k->v, &k->k, k->have_rec ? k->rec.home : 1u, know(k));
    const plink_vfeed_port_t port = {&k->v,   kv_baseline, kv_pause,   kv_event,
                                     kv_sound, kv_output,   kv_snapshot};
    plink_vfeed_init(&k->v.s.f, &port);
}

static void k_input(void *ctx, uint64_t now, const plink_addr_t *from, const uint8_t *f, size_t len)
{
    kp_t *k = ctx;
    if (len > 3u && f[3] < TYPES) {
        k->got[f[3]]++;
    }
    plink_keypad_input(&k->k, clock_of(now, k->off), from, f, len);
    observe(k);
}

static void k_tick(void *ctx, uint64_t now)
{
    kp_t *k = ctx;
    const uint64_t t = clock_of(now, k->off);
    if (k->resolve_due) {
        /* mDNS answered: the master is where it was */
        k->resolve_due = false;
        plink_keypad_master_addr(&k->k, t, maddr(), false);
    }
    plink_keypad_tick(&k->k, t);
    observe(k);
}

static uint64_t k_next(void *ctx)
{
    const kp_t *k = ctx;
    uint64_t t = min64(plink_keypad_next(&k->k), v_next(&k->v));
    if (k->resolve_due) {
        t = min64(t, know(k));
    }
    return net_of(t, k->off);
}

static bool kp_send(void *ctx, const plink_addr_t *to, const uint8_t *frame, size_t len)
{
    const kp_t *k = ctx;
    return plink_net_send(&R.net, k->node, to, frame, len);
}

static bool kp_store(void *ctx, const plink_keypad_rec_t *rec)
{
    kp_t *k = ctx;
    if (rec != NULL) {
        k->rec = *rec;
        k->have_rec = true;
        k->writes++;
    } else {
        memset(&k->rec, 0, sizeof k->rec);
        k->have_rec = false;
        k->erases++;
    }
    return true;
}

static void kp_resolve(void *ctx)
{
    kp_t *k = ctx;
    k->resolve_due = true; /* answered later, as mDNS is */
    k->resolves++;
}

static kp_t *kp_add(void)
{
    TEST_ASSERT_TRUE(R.nkp < KP_MAX);
    kp_t *k = &R.kp[R.nkp];
    const uint8_t ip[4] = {192u, 0u, 2u, (uint8_t)(21u + R.nkp)};
    k->name = NAMES[R.nkp];
    k->node = plink_net_node(&R.net, ip, k, k_input, k_tick, k_next);
    R.net.node[k->node].down = true; /* until it is switched on */
    R.nkp++;
    return k;
}

/* Switched on: a new manager from what its NVS holds, a new clock, new random numbers, its
 * memory blank; discovery finds the master at once. */
static void kp_boot(kp_t *k)
{
    const unsigned i = (unsigned)(k - R.kp);
    k->reused = k->reused || k->tp.reused;
    k->boots++;
    plink_test_port_init(&k->tp, (0x4B500000ull + 0x100ull * i + k->boots) ^ SEED);
    k->tp.only_dir = R.damaged ? PLINK_DIR_KEYPAD : 0u;
    k->c = plink_test_port(&k->tp);
    k->off = (int64_t)(4000u + 777u * i) - (int64_t)NOW;
    memset(k->blob, 0, sizeof k->blob);
    memset(&k->cfg, 0, sizeof k->cfg);
    memset(&k->port, 0, sizeof k->port);
    k->port.ctx = k;
    k->port.send = kp_send;
    k->port.store = kp_store;
    k->port.resolve = kp_resolve;
    plink_keypad_init(&k->k, &k->c, &k->port, k->have_rec ? &k->rec : NULL, k->blob, sizeof k->blob,
                      &k->cfg, PLINK_KIND_PANEL, &CAP, k->caps, k->name, "1.4.0", know(k));
    k->resolve_due = false;
    plink_keypad_info_t in;
    plink_keypad_info(&k->k, know(k), &in);
    k->state = in.state;
    k->n_path = 0u;
    memset(k->path, 0, sizeof k->path);
    k->path[k->n_path++] = in.state == PLINK_LINK_NOT_PAIRED ? 'N' : 'C';
    k->seen_at = 0u;
    k->cd_have = false;
    if (k->v.on) {
        kv_start(k); /* its voice starts again too, knowing nothing */
    }
    R.net.node[k->node].down = false;
    if (k->have_rec) {
        plink_keypad_master_addr(&k->k, know(k), maddr(), false);
    }
}

static kp_t *kp_new(void)
{
    kp_t *k = kp_add();
    kp_boot(k);
    return k;
}

static void kp_off(kp_t *k)
{
    R.net.node[k->node].down = true;
}

/* ---- what the keypads show ------------------------------------------------------------------- */

static bool part_same(const alarm_partition_status_t *m, uint64_t mtaken, uint64_t mt,
                      const alarm_partition_status_t *k, uint64_t ktaken, uint64_t kt)
{
    return m->state == k->state && m->level == k->level && m->arm_flags == k->arm_flags &&
           m->armed_by == k->armed_by && m->arm_source.kind == k->arm_source.kind &&
           m->arm_source.id == k->arm_source.id && m->ready_levels == k->ready_levels &&
           m->ready == k->ready && m->chime == k->chime && m->walk_test == k->walk_test &&
           m->triggered == k->triggered && m->exit_error == k->exit_error &&
           m->trouble_unack == k->trouble_unack && m->sounder == k->sounder && m->bell == k->bell &&
           near(left_now(m->delay_remaining_ms, mtaken, mt),
                left_now(k->delay_remaining_ms, ktaken, kt)) &&
           m->memory == k->memory && m->faults == k->faults && m->bypassed == k->bypassed &&
           m->alarms == k->alarms && m->troubles == k->troubles &&
           near(left_now(m->walk_test_remaining_ms, mtaken, mt),
                left_now(k->walk_test_remaining_ms, ktaken, kt)) &&
           (m->walk_started_ms != 0u) == (k->walk_started_ms != 0u) && m->walk_seq == k->walk_seq &&
           m->walk_tested == k->walk_tested && m->walk_total == k->walk_total &&
           m->walk_flags == k->walk_flags && m->walk_started_by == k->walk_started_by &&
           m->walk_ended_by == k->walk_ended_by && m->walk_end_how == k->walk_end_how &&
           m->walk_started_src.kind == k->walk_started_src.kind &&
           m->walk_started_src.id == k->walk_started_src.id;
}

/* LINK-002: the keypad's snapshot says what the master's does, in every field a screen shows,
 * each countdown read on its own side's clock; KPD-008: the lockout is this keypad's own. */
static bool snap_same(const kp_t *k)
{
    const alarm_snapshot_t *m = &R.snap;
    const alarm_snapshot_t *s = plink_keypad_snapshot(&k->k);
    const uint64_t mt = mnow(), kt = know(k);
    if (m->partitions != s->partitions || m->sys_troubles != s->sys_troubles ||
        m->sys_troubles_restored != s->sys_troubles_restored ||
        m->dev_troubles != s->dev_troubles || m->default_codes != s->default_codes ||
        m->program != s->program || m->exit_sounds != s->exit_sounds ||
        m->zones_used != s->zones_used || m->last_event_seq != s->last_event_seq ||
        !near(left_now(m->recovery_left_ms, m->taken_ms, mt),
              left_now(s->recovery_left_ms, s->taken_ms, kt))) {
        return false;
    }
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if ((m->partitions & (1u << p)) != 0u &&
            !part_same(&m->part[p], m->taken_ms, mt, &s->part[p], s->taken_ms, kt)) {
            return false;
        }
    }
    for (unsigned z = 0; z < m->zones_used; z++) {
        const alarm_zone_status_t *a = &m->zone[z], *b = &s->zone[z];
        if (a->flags != b->flags || a->trouble != b->trouble || a->walk != b->walk ||
            a->walk_first_s != b->walk_first_s || a->walk_trips != b->walk_trips) {
            return false;
        }
    }
    const alarm_source_t src = {ALARM_SRC_KEYPAD, dev(k)};
    const uint64_t lock = alarm_engine_lockout_left(&R.sim.eng, &src, mt);
    const uint64_t shown = s->local_lockout_until > kt ? s->local_lockout_until - kt : 0u;
    return near(lock, shown);
}

/* The keypad shows the master as it is: ONLINE in the session the master holds for it, the
 * master's configuration copy installed, its newest event, the same snapshot. */
static bool synced(const kp_t *k)
{
    if (R.net.node[k->node].down || !k->have_rec) {
        return false;
    }
    plink_keypad_info_t in;
    plink_keypad_info(&k->k, know(k), &in);
    const uint8_t d = dev(k);
    if (in.state != PLINK_LINK_ONLINE || d < PLINK_DEVICE_FIRST || d > PLINK_DEVICE_LAST ||
        !k->k.live.valid || k->k.tentative) {
        return false;
    }
    const plink_peer_t *p = &R.m.peer[d - 1u];
    if (!p->used || !p->live.valid || p->live.sid != k->k.live.sid) {
        return false;
    }
    const alarm_config_t *c = plink_keypad_config(&k->k);
    if (c == NULL || k->k.cfg_installed_crc != R.blob_crc || memcmp(c, &R.ref, sizeof R.ref) != 0) {
        return false;
    }
    alarm_event_t newest;
    if (R.n_ev > 0u &&
        (plink_keypad_events(&k->k, &newest, 1u) != 1u || newest.seq != R.ev[R.n_ev - 1u].seq)) {
        return false; /* its newest event isn't the master's yet */
    }
    return snap_same(k);
}

static bool c_synced(void *ctx)
{
    return synced(ctx);
}

static bool c_all_synced(void *ctx)
{
    (void)ctx;
    for (unsigned i = 0; i < R.nkp; i++) {
        if (!synced(&R.kp[i])) {
            return false;
        }
    }
    return true;
}

static uint8_t kstate(const kp_t *k)
{
    plink_keypad_info_t in;
    plink_keypad_info(&k->k, know(k), &in);
    return in.state;
}

/* The home partition's state on the keypad's screen. */
static uint8_t shows(const kp_t *k)
{
    plink_keypad_info_t in;
    plink_keypad_info(&k->k, know(k), &in);
    if (in.home < 1u || in.home > ALARM_MAX_PARTITIONS) {
        return 0xFFu;
    }
    return (uint8_t)plink_keypad_snapshot(&k->k)->part[in.home - 1u].state;
}

typedef struct {
    const kp_t *k;
    uint8_t state;
} want_t;

static bool c_shows(void *ctx)
{
    const want_t *w = ctx;
    return shows(w->k) == w->state;
}

static bool c_state(void *ctx)
{
    const want_t *w = ctx;
    return kstate(w->k) == w->state;
}

static bool c_rec(void *ctx)
{
    return ((const kp_t *)ctx)->have_rec;
}

typedef struct {
    uint8_t device;
    bool up;
} edge_t;

static bool c_edge(void *ctx)
{
    const edge_t *e = ctx;
    return edges(e->device, e->up) > 0u;
}

/* The keypad's events are the master's newest, newest first. */
static void check_events(const kp_t *k)
{
    static alarm_event_t got[PLINK_EVENTS_KEPT];
    const size_t n = plink_keypad_events(&k->k, got, PLINK_EVENTS_KEPT);
    TEST_ASSERT_TRUE(n > 0u);
    TEST_ASSERT_TRUE(n <= R.n_ev);
    for (size_t i = 0; i < n; i++) {
        const alarm_event_t *e = &R.ev[R.n_ev - 1u - i];
        TEST_ASSERT_EQUAL_UINT32(e->seq, got[i].seq);
        TEST_ASSERT_EQUAL_INT((int)e->type, (int)got[i].type);
        TEST_ASSERT_EQUAL_UINT8(e->partition, got[i].partition);
        TEST_ASSERT_EQUAL_UINT16(e->zone, got[i].zone);
        TEST_ASSERT_EQUAL_UINT8(e->user, got[i].user);
        TEST_ASSERT_EQUAL_UINT8(e->source.kind, got[i].source.kind);
        TEST_ASSERT_EQUAL_UINT16(e->source.id, got[i].source.id);
        TEST_ASSERT_EQUAL_UINT8(e->arg, got[i].arg);
        TEST_ASSERT_EQUAL_UINT8(e->arg2, got[i].arg2);
    }
}

/* ---- running --------------------------------------------------------------------------------- */

static void run_ms(uint64_t ms)
{
    plink_net_run(&R.net, ms);
}

static bool run_until(uint64_t ms, bool (*cond)(void *ctx), void *ctx)
{
    return plink_net_run_until(&R.net, ms, cond, ctx);
}

/* Frames of a type the master sent to node `to` (in the network's log since it was cleared). */
static unsigned m_sent_to(int to, uint8_t type)
{
    unsigned c = 0;
    for (size_t i = 0; i < R.net.log_count; i++) {
        const plink_net_seen_t *s = &R.net.log[i];
        c += s->from == R.mnode && s->to == to && s->type == type;
    }
    return c;
}

/* Frames node `from` sent (in the log since it was cleared). */
static unsigned sent_by(int from)
{
    unsigned c = 0;
    for (size_t i = 0; i < R.net.log_count; i++) {
        c += R.net.log[i].from == from;
    }
    return c;
}

/* ---- pairing --------------------------------------------------------------------------------- */

static bool c_compare(void *ctx)
{
    const kp_t *k = ctx;
    plink_pair_info_t a, b;
    plink_keypad_pair_info(&k->k, know(k), &a);
    plink_master_pair_info(&R.m, mnow(), &b);
    return a.state == PLINK_PS_COMPARE && b.state == PLINK_PS_COMPARE;
}

/* The installer opens pairing at the master, a person starts it on keypad k, both compare the
 * code and say yes; then the keypad syncs. */
static void pair_kp(kp_t *k)
{
    TEST_ASSERT_TRUE(plink_master_pair_open(&R.m, mnow()));
    plink_keypad_pair_start(&k->k, know(k), maddr(), false, plink_master_net_id(&R.m));
    TEST_ASSERT_TRUE(run_until(15000u, c_compare, k));
    plink_pair_info_t a, b;
    plink_keypad_pair_info(&k->k, know(k), &a);
    plink_master_pair_info(&R.m, mnow(), &b);
    /* LINK-004: both screens show the same six digits */
    TEST_ASSERT_EQUAL_UINT(6u, strlen(a.code));
    TEST_ASSERT_EQUAL_STRING(a.code, b.code);
    plink_keypad_pair_confirm(&k->k, know(k), true);
    plink_master_pair_decide(&R.m, mnow(), true);
    TEST_ASSERT_TRUE(run_until(10000u, c_rec, k));
    TEST_ASSERT_TRUE(run_until(30000u, c_synced, k));
}

/* ---- the keypads' commands ------------------------------------------------------------------- */

static uint32_t kcmd(kp_t *k, uint8_t op, alarm_cmd_type_t type, uint8_t part,
                     alarm_arm_level_t level, const char *code)
{
    plink_cmd_t c;
    memset(&c, 0, sizeof c);
    c.op = op;
    c.cmd.type = type;
    c.cmd.partition = part;
    c.cmd.level = level;
    c.cmd.panic = ALARM_PANIC_MEDICAL;
    snprintf(c.cmd.code, sizeof c.cmd.code, "%s", code);
    return plink_keypad_command(&k->k, know(k), &c);
}

typedef struct {
    const kp_t *k;
    uint32_t req;
} req_t;

static bool c_answered(void *ctx)
{
    const req_t *w = ctx;
    return plink_keypad_command_state(&w->k->k, w->req, NULL) != PLINK_CMD_PENDING;
}

/* Runs until command req of keypad k is no longer pending, at most ms; its state and answer. */
static plink_cmd_state_t kwait(kp_t *k, uint32_t req, uint64_t ms, plink_result_t *res)
{
    req_t w = {k, req};
    (void)run_until(ms, c_answered, &w);
    plink_result_t r;
    memset(&r, 0, sizeof r);
    const plink_cmd_state_t st = plink_keypad_command_state(&k->k, req, &r);
    if (res != NULL) {
        *res = r;
    }
    return st;
}

/* A command from k answered within ms with `expect`, run once by the engine - or not at all when
 * the link refused it (NOT_AUTHORIZED: another partition than the keypad's home, LINK-027). */
static void kdo(kp_t *k, uint8_t op, alarm_cmd_type_t type, uint8_t part, alarm_arm_level_t level,
                const char *code, uint64_t ms, alarm_result_t expect)
{
    const uint32_t req = kcmd(k, op, type, part, level, code);
    TEST_ASSERT_NOT_EQUAL_UINT32(0u, req);
    plink_result_t res;
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_DONE, kwait(k, req, ms, &res));
    EXPECT_RESULT(expect, (alarm_result_t)res.result);
    TEST_ASSERT_EQUAL_UINT(expect == ALARM_ERR_NOT_AUTHORIZED ? 0u : 1u, ran_req(dev(k), req));
    plink_keypad_command_release(&k->k, req);
}

/* ---- the master restarting ------------------------------------------------------------------- */

/* A new plink_master_t from its stored record, as at power-up: the engine starts again from its
 * persisted state with the same configuration, a new clock, new random numbers; nothing else of
 * the old one is left (the worker's queue went with it). */
static void master_restart(void)
{
    TEST_ASSERT_TRUE(R.have_stored);
    R.m_reused = R.m_reused || R.tpm.reused;
    CFG_TMP = R.sim.cfg;
    const bool had = R.sim.n_persist > 0u;
    const alarm_runtime_state_t st = R.sim.persisted;
    R.n_pend = 0u;
    sim_start(&R.sim, &CFG_TMP, had ? &st : NULL);
    R.sim.eng.ports.get_epoch_ms = rig_wall_clock;
    R.ev_fwd = R.snd_fwd = R.out_fwd = 0u;
    R.m_off = (int64_t)R.sim.now - (int64_t)NOW;
    if (R.mv.on) {
        /* its voice starts again with it: the first commit (the pump below) is its baseline */
        v_start(&R.mv, NULL, R.mv.s.a.keypad, mnow());
    }
    plink_test_port_init(&R.tpm, (0x3A570000ull + NOW) ^ SEED);
    R.tpm.only_dir = R.damaged ? PLINK_DIR_MASTER : 0u;
    R.cm = plink_test_port(&R.tpm);
    plink_master_init(&R.m, &R.cm, &R.port, &R.stored, "Harbor House", "a1b2c3", R.m_caps,
                      mnow());
    plink_master_config(&R.m, R.blob, R.blob_len, R.blob_crc);
    plink_master_time(&R.m, mnow(), epoch_now(), "UTC0");
    pump(mnow());
    R.net.node[R.mnode].down = false;
}

/* The keypads' copy of the engine's configuration, handed to the master. */
static void remake_blob(void)
{
    TEST_ASSERT_EQUAL_INT(ALARM_OK, plink_cfg_for_keypads(&R.sim.cfg, &R.scratch, R.blob,
                                                          sizeof R.blob, &R.blob_len, &R.blob_crc));
    TEST_ASSERT_EQUAL_INT(ALARM_OK, plink_cfg_decode(R.blob, R.blob_len, &R.ref));
    plink_master_config(&R.m, R.blob, R.blob_len, R.blob_crc);
}

/* From now on frames may be damaged: each side's port watches only its own seals (a frame whose
 * tag failed may be opened again under the same nonce, which is harmless). */
static void damage_on(void)
{
    R.damaged = true;
    R.tpm.only_dir = PLINK_DIR_MASTER;
    for (unsigned i = 0; i < R.nkp; i++) {
        R.kp[i].tp.only_dir = PLINK_DIR_KEYPAD;
    }
}

/* ---- someone with a keypad's keys ------------------------------------------------------------ */

static void h_seal(uint8_t type, const uint8_t *p, size_t n)
{
    size_t len = 0;
    if (plink_session_seal(R.h.c, &R.h.s, type, R.h.rec.net_id, R.h.rec.device, false, p, n, FBUF,
                           sizeof FBUF, &len)) {
        (void)plink_net_send(&R.net, R.xnode, maddr(), FBUF, len);
    }
}

static void h_cmd(uint32_t req, alarm_cmd_type_t type, uint8_t part, const char *code)
{
    plink_cmd_t c;
    memset(&c, 0, sizeof c);
    c.req = req;
    c.op = PLINK_OP_COMMAND;
    c.cmd.type = type;
    c.cmd.partition = part;
    c.cmd.level = ALARM_LEVEL_AWAY;
    snprintf(c.cmd.code, sizeof c.cmd.code, "%s", code);
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_cmd(&c, PBUF, sizeof PBUF, &n));
    h_seal(PLINK_T_CMD, PBUF, n);
}

/* One burst every 5 ms: new commands (a wrong code, another partition), a copy of an old one,
 * the three requests, and every other burst a fresh HELLO. */
static void x_tick(void *ctx, uint64_t now)
{
    (void)ctx;
    hostile_t *h = &R.h;
    if (!h->on || now < h->next) {
        return;
    }
    h->next = now + 5u;
    h->bursts++;
    h_cmd(h->copy_req, ALARM_CMD_DISARM, h->rec.home, "7531");
    h_cmd(++h->req, ALARM_CMD_ARM, h->rec.home, "8642");
    h_cmd(++h->req, ALARM_CMD_ARM, 2u, "123456");
    h->cmds += 3u;
    size_t n = 0;
    const plink_cfg_req_t cq = {R.blob_crc, 0u, 1000u};
    TEST_ASSERT_TRUE(plink_enc_cfg_req(&cq, PBUF, sizeof PBUF, &n));
    h_seal(PLINK_T_CFG_REQ, PBUF, n);
    const plink_ev_req_t eq = {0u, 50u};
    TEST_ASSERT_TRUE(plink_enc_ev_req(&eq, PBUF, sizeof PBUF, &n));
    h_seal(PLINK_T_EV_REQ, PBUF, n);
    const plink_zones_req_t zq = {1u, ALARM_MAX_ZONES};
    TEST_ASSERT_TRUE(plink_enc_zones_req(&zq, PBUF, sizeof PBUF, &n));
    h_seal(PLINK_T_ZONES_REQ, PBUF, n);
    if (h->hellos && (h->bursts & 1u) != 0u &&
        plink_hs_hello(h->c, h->rec.k_pair, h->rec.net_id, h->rec.device, &CAP, 0u, &h->hs)) {
        (void)plink_net_send(&R.net, R.xnode, maddr(), h->hs.hello, h->hs.hello_len);
    }
}

static uint64_t x_next(void *ctx)
{
    (void)ctx;
    return R.h.on ? R.h.next : UINT64_MAX;
}

static void x_input(void *ctx, uint64_t now, const plink_addr_t *from, const uint8_t *f, size_t len)
{
    (void)ctx;
    (void)now;
    (void)from;
    if (len > 3u && f[3] < TYPES) {
        R.h.got[f[3]]++;
    }
}

/* ---- set-up ---------------------------------------------------------------------------------- */

void setUp(void)
{
    memset(&R, 0, sizeof R);
    const char *seed = getenv("PLINK_LINK_SEED");
    SEED = seed != NULL ? strtoull(seed, NULL, 0) * 0x9E3779B97F4A7C15ull : 0u;
    plink_net_init(&R.net, 0x11AB5EEDull ^ SEED);
    R.net.now_ms = 50000u;
    sim_start(&R.sim, NULL, NULL);
    R.sim.eng.ports.get_epoch_ms = rig_wall_clock;
    R.m_off = (int64_t)R.sim.now - (int64_t)NOW;
    R.rng = 0x5EED0001ull ^ SEED;
    plink_test_port_init(&R.tpm, 0x3A57u ^ SEED);
    R.cm = plink_test_port(&R.tpm);
    R.port.ctx = NULL;
    R.port.send = p_send;
    R.port.store = p_store;
    R.port.run = p_run;
    R.port.reachable = p_reachable;
    R.port.checkin = p_checkin;
    R.port.devices_taken = p_taken;
    R.port.held_changed = p_held;
    R.port.lockout_left = p_lockout;
    R.port.events_after = p_events_after;
    R.port.note = p_note;
    plink_master_init(&R.m, &R.cm, &R.port, NULL, "Harbor House", "a1b2c3", R.m_caps, mnow());
    R.mnode = plink_net_node(&R.net, IP_M, NULL, m_input, m_tick, m_next);
    R.xnode = plink_net_node(&R.net, IP_X, NULL, x_input, x_tick, x_next);
    R.run_mode = RUN_NOW;
    remake_blob();
    plink_master_time(&R.m, mnow(), epoch_now(), "UTC0");
    pump(mnow());
}

void tearDown(void)
{
    /* LINK-003, LINK-015: no AES-CCM key and nonce was used twice, on any side, in any boot. */
    TEST_ASSERT_FALSE_MESSAGE(R.m_reused || R.tpm.reused, "the master used a key and nonce twice");
    for (unsigned i = 0; i < R.nkp; i++) {
        const kp_t *k = &R.kp[i];
        TEST_ASSERT_FALSE_MESSAGE(k->reused || k->tp.reused, "a keypad used a key and nonce twice");
        /* LINK-008 */
        TEST_ASSERT_FALSE_MESSAGE(k->online_bad, "a keypad was ONLINE without status or config");
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, R.sent_in_run, "a frame went out from inside port.run");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, R.busy, "two commands of one keypad ran at once");
    /* LINK-018: no command ran twice (the same keypad, request id and command). */
    for (unsigned i = 0; i < R.n_ran; i++) {
        for (unsigned j = i + 1u; j < R.n_ran; j++) {
            TEST_ASSERT_FALSE_MESSAGE(
                R.ran[i].device == R.ran[j].device && R.ran[i].req == R.ran[j].req &&
                    memcmp(R.ran[i].digest, R.ran[j].digest, PLINK_SHA256_LEN) == 0,
                "a command ran twice");
        }
    }
}

/* ---- pairing and the first sync -------------------------------------------------------------- */

/* LINK-004, LINK-016: pairing from scratch - both screens show the same code with the other
 * side's name and address, nothing is stored before both said yes; LINK-001: the keypad takes the
 * lowest number nobody holds; LINK-008: CONNECTING until a session, a status and the
 * configuration are in, then ONLINE; LINK-002: its snapshot is the master's in every displayed
 * field; LINK-019: its configuration is the master's scrubbed copy (no code material, duress shown
 * as standard); the master's newest events are there. */
static void test_pairing_from_scratch_reaches_online(void)
{
    /* Something in the master's log first: an arm and a disarm at its own touchscreen. */
    EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_MASTER));
    EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_MASTER));
    TEST_ASSERT_TRUE(R.n_ev >= 2u);
    R.taken = 0x01u; /* number 1: a Zigbee keypad */
    kp_t *a = kp_new();
    plink_keypad_info_t in;
    plink_keypad_info(&a->k, know(a), &in);
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_NOT_PAIRED, in.state);
    TEST_ASSERT_EQUAL_UINT8(PLINK_UNPAIRED_NEVER, in.unpaired_why);
    TEST_ASSERT_EQUAL_UINT16(0u, plink_master_net_id(&R.m));
    TEST_ASSERT_FALSE(plink_master_in_use(&R.m));

    TEST_ASSERT_TRUE(plink_master_pair_open(&R.m, mnow()));
    TEST_ASSERT_NOT_EQUAL(0u, plink_master_net_id(&R.m));
    TEST_ASSERT_TRUE(plink_master_in_use(&R.m));
    TEST_ASSERT_TRUE(plink_master_pairing(&R.m));
    plink_pair_info_t ki, mi;
    plink_master_pair_info(&R.m, mnow(), &mi);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_OPEN, mi.state);
    TEST_ASSERT_EQUAL_UINT32(PLINK_PAIR_WINDOW_MS / 1000u, mi.left_s);
    TEST_ASSERT_EQUAL_STRING("", mi.code);
    plink_keypad_pair_start(&a->k, know(a), maddr(), false, plink_master_net_id(&R.m));
    plink_keypad_pair_info(&a->k, know(a), &ki);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_OPEN, ki.state); /* its request sent */
    TEST_ASSERT_TRUE(run_until(15000u, c_compare, a));
    plink_keypad_pair_info(&a->k, know(a), &ki);
    plink_master_pair_info(&R.m, mnow(), &mi);
    TEST_ASSERT_EQUAL_UINT(6u, strlen(ki.code));
    TEST_ASSERT_EQUAL_STRING(ki.code, mi.code);
    TEST_ASSERT_EQUAL_STRING("Harbor House", ki.peer_name);
    TEST_ASSERT_EQUAL_STRING("Hall keypad", mi.peer_name);
    TEST_ASSERT_TRUE(plink_addr_equal(maddr(), &ki.peer));
    TEST_ASSERT_TRUE(plink_addr_equal(plink_net_addr(&R.net, a->node), &mi.peer));
    TEST_ASSERT_FALSE(a->have_rec);
    TEST_ASSERT_EQUAL_UINT8(0u, R.stored.count);

    /* The keypad says yes; nothing is stored until the installer does too. */
    plink_keypad_pair_confirm(&a->k, know(a), true);
    run_ms(500u);
    plink_keypad_pair_info(&a->k, know(a), &ki);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_WAITING, ki.state);
    plink_master_pair_info(&R.m, mnow(), &mi);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_COMPARE, mi.state);
    TEST_ASSERT_FALSE(a->have_rec);
    TEST_ASSERT_EQUAL_UINT8(0u, R.stored.count);
    plink_master_pair_decide(&R.m, mnow(), true);
    TEST_ASSERT_TRUE(run_until(5000u, c_rec, a));
    TEST_ASSERT_EQUAL_UINT8(2u, a->rec.device);
    TEST_ASSERT_EQUAL_UINT8(1u, a->rec.home);
    TEST_ASSERT_EQUAL_UINT16(plink_master_net_id(&R.m), a->rec.net_id);
    TEST_ASSERT_EQUAL_STRING("Harbor House", a->rec.master_name);
    TEST_ASSERT_EQUAL_STRING("a1b2c3", a->rec.panel_id);
    TEST_ASSERT_EQUAL_UINT32(0x02u, plink_master_held(&R.m));
    TEST_ASSERT_EQUAL_UINT32(0x02u, R.held);
    TEST_ASSERT_TRUE(run_until(15000u, c_synced, a));
    /* LINK-008: from pairing on the keypad went CONNECTING, then ONLINE, nothing else. */
    TEST_ASSERT_EQUAL_STRING("NCO", a->path);
    plink_keypad_pair_info(&a->k, know(a), &ki);
    plink_master_pair_info(&R.m, mnow(), &mi);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_DONE, ki.state);
    TEST_ASSERT_EQUAL_UINT8(2u, ki.device);
    TEST_ASSERT_EQUAL_UINT8(PLINK_PS_DONE, mi.state);
    TEST_ASSERT_EQUAL_UINT8(2u, mi.device);
    TEST_ASSERT_FALSE(plink_master_pairing(&R.m));

    /* Both records: the same pairing key; the master's no longer provisional after the first
     * handshake. */
    TEST_ASSERT_EQUAL_UINT8(1u, R.stored.count);
    TEST_ASSERT_EQUAL_UINT8(2u, R.stored.peer[0].device);
    TEST_ASSERT_EQUAL_UINT8(0u, R.stored.peer[0].flags & PLINK_PEER_PROVISIONAL);
    TEST_ASSERT_EQUAL_UINT8(PLINK_KIND_PANEL, R.stored.peer[0].kind);
    TEST_ASSERT_EQUAL_STRING("", R.stored.peer[0].name); /* "Keypad 2" until renamed */
    TEST_ASSERT_EQUAL_MEMORY(a->rec.k_pair, R.stored.peer[0].k_pair, PLINK_KEY_LEN);
    plink_peer_info_t pi[PLINK_KEYPADS_MAX];
    TEST_ASSERT_EQUAL_size_t(1u, plink_master_peers(&R.m, mnow(), pi, PLINK_KEYPADS_MAX));
    TEST_ASSERT_EQUAL_UINT8(2u, pi[0].device);
    TEST_ASSERT_TRUE(pi[0].online);
    TEST_ASSERT_FALSE(pi[0].provisional);
    TEST_ASSERT_FALSE(pi[0].down);
    TEST_ASSERT_TRUE(plink_addr_equal(plink_net_addr(&R.net, a->node), &pi[0].addr));
    TEST_ASSERT_EQUAL_STRING("1.4.0", pi[0].hb.fw);

    /* LINK-019: what the keypad holds is the scrubbed copy: no code hash of the master's, no
     * salt, the duress user a standard one, every name as the master has it. */
    const alarm_config_t *kc = plink_keypad_config(&a->k);
    TEST_ASSERT_NOT_NULL(kc);
    TEST_ASSERT_EQUAL_UINT32(1u, plink_keypad_config_seq(&a->k));
    for (unsigned i = 0; i < ALARM_CODE_SALT_LEN; i++) {
        TEST_ASSERT_EQUAL_UINT8(0u, kc->code_salt[i]);
    }
    unsigned users = 0;
    for (unsigned u = 0; u < ALARM_MAX_USERS; u++) {
        const alarm_user_t *mu = &R.sim.cfg.users[u];
        if (mu->authority == ALARM_AUTH_NONE) {
            continue;
        }
        users++;
        TEST_ASSERT_FALSE(memcmp(mu->code_hash, kc->users[u].code_hash, ALARM_CODE_HASH_LEN) == 0);
        TEST_ASSERT_EQUAL_STRING(mu->name, kc->users[u].name);
        TEST_ASSERT_EQUAL_INT(mu->authority == ALARM_AUTH_DURESS ? ALARM_AUTH_STANDARD
                                                                 : mu->authority,
                              kc->users[u].authority);
    }
    TEST_ASSERT_EQUAL_UINT(7u, users);
    TEST_ASSERT_EQUAL_INT(ALARM_AUTH_DURESS, R.sim.cfg.users[5].authority);
    for (unsigned z = 0; z < ALARM_MAX_ZONES; z++) {
        TEST_ASSERT_EQUAL_STRING(R.sim.cfg.zones[z].name, kc->zones[z].name);
        TEST_ASSERT_EQUAL_INT(R.sim.cfg.zones[z].type, kc->zones[z].type);
    }
    /* The master's newest events, in order; its wall clock and its reporting. */
    check_events(a);
    TEST_ASSERT_EQUAL_UINT32(R.ev[R.n_ev - 1u].seq, plink_keypad_snapshot(&a->k)->last_event_seq);
    TEST_ASSERT_TRUE(near((uint64_t)plink_keypad_epoch_ms(&a->k, know(a)), (uint64_t)epoch_now()));
    TEST_ASSERT_EQUAL_UINT8(PLINK_REPORT_ONLINE, plink_keypad_report(&a->k));
}

/* ---- commands -------------------------------------------------------------------------------- */

/* LINK-002, LINK-018: arm away from a keypad with a code (the master checks it), leave through the
 * front door; someone comes in: the keypad counts the exit and entry delays down with the
 * master, and the seconds it shows never go up through 150 ms of jitter and 10 % loss (UI-023);
 * disarm from the keypad. On a working network each answer and each change within a second
 * (LINK-024's protocol part); each command run once. */
static void test_arm_away_entry_countdown_and_disarm_from_a_keypad(void)
{
    kp_t *a = kp_new();
    pair_kp(a);
    R.net.jitter_ms = 150u;
    kdo(a, PLINK_OP_COMMAND, ALARM_CMD_ARM, 1u, ALARM_LEVEL_AWAY, CODE_USER, 1000u, ALARM_OK);
    TEST_ASSERT_EQUAL_INT(ALARM_STATE_EXIT_DELAY, sim_status(&R.sim, 1u).state);
    want_t w = {a, (uint8_t)ALARM_STATE_EXIT_DELAY};
    TEST_ASSERT_TRUE(run_until(1000u, c_shows, &w));
    R.watch = true;
    R.net.loss_pm = 100u;
    run_ms(5000u);
    m_zone(Z_FRONT, true); /* out through the front door */
    run_ms(3000u);
    m_zone(Z_FRONT, false);
    w.state = (uint8_t)ALARM_STATE_ARMED;
    TEST_ASSERT_TRUE(run_until(65000u, c_shows, &w));
    TEST_ASSERT_EQUAL_INT(ALARM_LEVEL_AWAY, sim_status(&R.sim, 1u).level);
    TEST_ASSERT_EQUAL_INT(ALARM_LEVEL_AWAY, plink_keypad_snapshot(&a->k)->part[0].level);
    run_ms(20000u);

    /* Someone comes in: the entry delay on both, the keypad's never rising. */
    m_zone(Z_FRONT, true);
    w.state = (uint8_t)ALARM_STATE_ENTRY_DELAY;
    TEST_ASSERT_TRUE(run_until(12000u, c_shows, &w)); /* (its first copies may all be lost) */
    for (unsigned i = 0; i < 150u; i++) {
        run_ms(100u);
        const alarm_partition_status_t *ms = &R.snap.part[0];
        const alarm_snapshot_t *ks = plink_keypad_snapshot(&a->k);
        TEST_ASSERT_EQUAL_INT(ms->state, ks->part[0].state);
        TEST_ASSERT_TRUE(near(left_now(ms->delay_remaining_ms, R.snap.taken_ms, mnow()),
                              left_now(ks->part[0].delay_remaining_ms, ks->taken_ms, know(a))));
    }
    m_zone(Z_FRONT, false);
    TEST_ASSERT_EQUAL_UINT(0u, a->cd_rises);
    R.net.loss_pm = 0u;
    kdo(a, PLINK_OP_COMMAND, ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_USER, 1000u, ALARM_OK);
    w.state = (uint8_t)ALARM_STATE_DISARMED;
    TEST_ASSERT_TRUE(run_until(1000u, c_shows, &w));
    EXPECT_STATE(&R.sim, 1u, ALARM_STATE_DISARMED);
    TEST_ASSERT_EQUAL_UINT(0u, a->cd_rises);
    TEST_ASSERT_EQUAL_UINT(2u, ran_dev(dev(a)));
    TEST_ASSERT_TRUE(run_until(15000u, c_synced, a));
}

/* KPD-008, LINK-018: five wrong codes at one keypad lock that keypad out: it shows its own
 * lockout within a second of the answer, the other keypad and the master's own touchscreen are
 * not locked, and a right code at the locked keypad is refused until the lockout is over. */
static void test_wrong_codes_lock_out_that_keypad_only(void)
{
    kp_t *a = kp_new();
    pair_kp(a);
    kp_t *b = kp_new();
    pair_kp(b);
    for (unsigned i = 0; i < 5u; i++) {
        const uint32_t req =
            kcmd(a, PLINK_OP_COMMAND, ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, "8642");
        TEST_ASSERT_NOT_EQUAL_UINT32(0u, req);
        plink_result_t res;
        TEST_ASSERT_EQUAL_INT(PLINK_CMD_DONE, kwait(a, req, 3000u, &res));
        TEST_ASSERT_TRUE(res.result == ALARM_ERR_INVALID_CODE ||
                         res.result == ALARM_ERR_LOCKED_OUT);
        plink_keypad_command_release(&a->k, req);
    }
    const alarm_source_t src = {ALARM_SRC_KEYPAD, dev(a)};
    TEST_ASSERT_TRUE(alarm_engine_lockout_left(&R.sim.eng, &src, mnow()) > 0u);
    /* The keypad shows its own lockout within a second, counted down on its own clock. */
    run_ms(1000u);
    const alarm_snapshot_t *as = plink_keypad_snapshot(&a->k);
    TEST_ASSERT_TRUE(as->local_lockout_until > know(a));
    TEST_ASSERT_TRUE(near(as->local_lockout_until - know(a),
                          alarm_engine_lockout_left(&R.sim.eng, &src, mnow())));
    /* Nobody else is locked out: the other keypad, the master's touchscreen. */
    TEST_ASSERT_EQUAL_UINT64(0u, plink_keypad_snapshot(&b->k)->local_lockout_until);
    TEST_ASSERT_EQUAL_UINT64(0u, R.snap.local_lockout_until);
    EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_USER));
    EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_USER));
    kdo(b, PLINK_OP_COMMAND, ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_USER, 1000u, ALARM_OK);
    kdo(b, PLINK_OP_COMMAND, ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_USER, 1000u, ALARM_OK);
    /* The locked keypad: even the right code is refused. */
    kdo(a, PLINK_OP_COMMAND, ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_USER, 3000u,
        ALARM_ERR_LOCKED_OUT);
    /* (a STATUS changed within 100 ms of the last one waits out those 100 ms: LINK-006) */
    TEST_ASSERT_TRUE(run_until(1000u, c_all_synced, NULL));
    /* Over after the configured time: the right code works again. */
    run_ms((uint64_t)R.sim.cfg.opt.lockout_s * 1000u + 1000u);
    TEST_ASSERT_EQUAL_UINT64(0u, alarm_engine_lockout_left(&R.sim.eng, &src, mnow()));
    TEST_ASSERT_TRUE(plink_keypad_snapshot(&a->k)->local_lockout_until <= know(a));
    kdo(a, PLINK_OP_COMMAND, ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_USER, 1000u, ALARM_OK);
    TEST_ASSERT_TRUE(run_until(2000u, c_all_synced, NULL));
}

/* LINK-027: a keypad's command or code check for a partition other than its home is answered
 * NOT_AUTHORIZED by the link, and nothing reaches the engine. */
static void test_command_for_another_partition_is_refused(void)
{
    kp_t *a = kp_new();
    pair_kp(a);
    const unsigned ran0 = R.n_ran, ev0 = R.n_ev;
    const uint32_t req =
        kcmd(a, PLINK_OP_COMMAND, ALARM_CMD_ARM, 2u, ALARM_LEVEL_AWAY, CODE_MASTER);
    plink_result_t res;
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_DONE, kwait(a, req, 1000u, &res));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, (alarm_result_t)res.result);
    const uint32_t req2 =
        kcmd(a, PLINK_OP_AUTH, ALARM_CMD_CODE_CHECK, 3u, ALARM_LEVEL_NONE, CODE_MASTER);
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_DONE, kwait(a, req2, 1000u, &res));
    EXPECT_RESULT(ALARM_ERR_NOT_AUTHORIZED, (alarm_result_t)res.result);
    TEST_ASSERT_EQUAL_UINT(ran0, R.n_ran);
    TEST_ASSERT_EQUAL_UINT(ev0, R.n_ev);
    EXPECT_STATE(&R.sim, 1u, ALARM_STATE_DISARMED);
    /* Its home partition: the code check runs and names the user. */
    const uint32_t req3 =
        kcmd(a, PLINK_OP_AUTH, ALARM_CMD_CODE_CHECK, 1u, ALARM_LEVEL_NONE, CODE_MASTER);
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_DONE, kwait(a, req3, 1000u, &res));
    EXPECT_RESULT(ALARM_OK, (alarm_result_t)res.result);
    TEST_ASSERT_EQUAL_UINT8(2u, res.user);
    TEST_ASSERT_EQUAL_UINT(ran0 + 1u, R.n_ran);
}

/* The engine started again, before anything ran, with a second partition: its door (zone 21) and
 * user 8 (code 2468) who has only partition 2. */
#define Z_P2_DOOR 21u
#define CODE_P2   "2468"

static void two_partitions(void)
{
    sim_config_basic(&CFG_TMP);
    CFG_TMP.partitions[1].enabled = true;
    sim_set_zone(&CFG_TMP, Z_P2_DOOR, ALARM_ZT_ENTRY_EXIT_1, 2u);
    sim_set_user(&CFG_TMP, 8u, ALARM_AUTH_STANDARD, 0x02u, CODE_P2);
    sim_start(&R.sim, &CFG_TMP, NULL);
    R.sim.eng.ports.get_epoch_ms = rig_wall_clock;
    TEST_ASSERT_EQUAL_UINT64(R.sim.now, mnow());
    R.ev_fwd = R.snd_fwd = R.out_fwd = 0u;
    R.n_ev = 0u;
    remake_blob();
    pump(mnow());
}

/* LINK-027 and docs/features/11 "Roles on a P4 panel" (home partition, sounds): a keypad works
 * its home partition only. Moved to another partition at the master, it learns so from its next
 * STATUS (and keeps it in its record), and its commands follow; a one-shot sound goes to the
 * keypads whose home it concerns; every keypad's snapshot shows every partition, so a keypad of
 * partition 2 sees partition 1's fire. */
static void test_home_partition_moves_and_sounds_follow_it(void)
{
    two_partitions();
    kp_t *a = kp_new();
    pair_kp(a);
    kp_t *b = kp_new();
    pair_kp(b);
    TEST_ASSERT_EQUAL_UINT8(0x03u, R.snap.partitions);
    TEST_ASSERT_EQUAL_UINT8(1u, b->rec.home);
    const unsigned writes = b->writes;
    TEST_ASSERT_TRUE(plink_master_set_home(&R.m, mnow(), dev(b), 2u));
    TEST_ASSERT_EQUAL_UINT8(2u, R.stored.peer[1].home);
    run_ms(1000u);
    plink_keypad_info_t in;
    plink_keypad_info(&b->k, know(b), &in);
    TEST_ASSERT_EQUAL_UINT8(2u, in.home);
    TEST_ASSERT_EQUAL_UINT8(2u, b->rec.home); /* in its own record too */
    TEST_ASSERT_EQUAL_UINT(writes + 1u, b->writes);
    TEST_ASSERT_TRUE(run_until(1000u, c_all_synced, NULL));
    /* Its commands follow its home. */
    kdo(b, PLINK_OP_COMMAND, ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_USER, 1000u,
        ALARM_ERR_NOT_AUTHORIZED);
    kdo(b, PLINK_OP_COMMAND, ALARM_CMD_ARM, 2u, ALARM_LEVEL_STAY, CODE_P2, 1000u, ALARM_OK);
    EXPECT_STATE(&R.sim, 2u, ALARM_STATE_EXIT_DELAY);
    EXPECT_STATE(&R.sim, 1u, ALARM_STATE_DISARMED);
    want_t w = {b, (uint8_t)ALARM_STATE_EXIT_DELAY};
    TEST_ASSERT_TRUE(run_until(1000u, c_shows, &w));
    TEST_ASSERT_EQUAL_INT(ALARM_STATE_DISARMED, shows(a));
    kdo(b, PLINK_OP_COMMAND, ALARM_CMD_DISARM, 2u, ALARM_LEVEL_NONE, CODE_P2, 1000u, ALARM_OK);
    kdo(a, PLINK_OP_COMMAND, ALARM_CMD_ARM, 2u, ALARM_LEVEL_STAY, CODE_P2, 1000u,
        ALARM_ERR_NOT_AUTHORIZED);
    /* A chime on partition 1 reaches a, not b; a wrong code on partition 2 beeps on b only. */
    EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_CHIME, 1u, ALARM_LEVEL_NONE, CODE_USER));
    run_ms(500u);
    const unsigned a_chime = a->snd[ALARM_SOUND_CHIME], b_chime = b->snd[ALARM_SOUND_CHIME];
    m_zone(Z_FRONT, true);
    run_ms(500u);
    m_zone(Z_FRONT, false);
    run_ms(500u);
    TEST_ASSERT_TRUE(R.snd[1][ALARM_SOUND_CHIME] > 0u);
    TEST_ASSERT_TRUE(a->snd[ALARM_SOUND_CHIME] > a_chime);
    TEST_ASSERT_EQUAL_UINT(b_chime, b->snd[ALARM_SOUND_CHIME]);
    const unsigned a_err = a->snd[ALARM_SOUND_ERROR], b_err = b->snd[ALARM_SOUND_ERROR];
    const unsigned e2 = R.snd[2][ALARM_SOUND_ERROR];
    kdo(b, PLINK_OP_COMMAND, ALARM_CMD_DISARM, 2u, ALARM_LEVEL_NONE, "1357", 1000u,
        ALARM_ERR_INVALID_CODE);
    run_ms(500u);
    TEST_ASSERT_TRUE(R.snd[2][ALARM_SOUND_ERROR] > e2);
    TEST_ASSERT_TRUE(b->snd[ALARM_SOUND_ERROR] > b_err);
    TEST_ASSERT_EQUAL_UINT(a_err, a->snd[ALARM_SOUND_ERROR]);
    /* Partition 1's fire shows on b's screen too (it plays fire and CO from any partition). */
    m_zone(Z_SMOKE, true);
    TEST_ASSERT_TRUE(run_until(1000u, c_all_synced, NULL));
    TEST_ASSERT_EQUAL_INT(ALARM_SOUND_FIRE, R.snap.part[0].sounder);
    TEST_ASSERT_EQUAL_INT(ALARM_SOUND_FIRE, plink_keypad_snapshot(&b->k)->part[0].sounder);
    TEST_ASSERT_EQUAL_INT(ALARM_SOUND_FIRE, plink_keypad_snapshot(&a->k)->part[0].sounder);
}

/* LINK-002 (docs/features/11 "Event record": EV_REQ fills gaps): one engine call that makes two
 * events - an arm that bypasses an open window - is pushed as two EVENTS; the first is lost on the
 * way and the second arrives. The keypad sees the hole and pulls what it lacks, so its events are
 * the master's, none missing. */
static void test_a_lost_event_push_is_filled_in(void)
{
    kp_t *a = kp_new();
    pair_kp(a);
    m_zone(Z_WINDOW, true);
    TEST_ASSERT_TRUE(run_until(2000u, c_synced, a));
    const unsigned n0 = R.n_ev;
    R.net.lose_type = PLINK_T_EVENTS;
    R.net.lose_type_count = 1u;
    alarm_cmd_t c;
    memset(&c, 0, sizeof c);
    c.type = ALARM_CMD_ARM;
    c.partition = 1u;
    c.level = ALARM_LEVEL_STAY;
    c.arm_flags = ALARM_ARM_BYPASS_FAULTED;
    snprintf(c.code, sizeof c.code, "%s", CODE_USER);
    const alarm_source_t src = {ALARM_SRC_KEYPAD, ALARM_KEYPAD_LOCAL};
    sync_engine(mnow());
    EXPECT_RESULT(ALARM_OK, alarm_engine_command(&R.sim.eng, &c, &src, R.sim.now, NULL));
    pump(mnow());
    TEST_ASSERT_EQUAL_UINT(n0 + 2u, R.n_ev);
    TEST_ASSERT_EQUAL_UINT8(0u, R.net.lose_type); /* the first push was lost */
    TEST_ASSERT_TRUE(run_until(2000u, c_synced, a));
    run_ms(1000u);
    check_events(a);
}

typedef struct {
    const kp_t *k;
    size_t floor; /* the events the keypad held */
    bool dropped;
} ring_watch_t;

static bool c_ring_dropped(void *ctx)
{
    ring_watch_t *w = ctx;
    w->dropped = w->dropped || w->k->k.ev_count < w->floor;
    return false; /* watched to the end */
}

/* LINK-015, docs/features/11 "Event record": an event pushed under one session ahead of the
 * snapshot that carries it (the alarm task hands events in as it makes them; its snapshot comes
 * after the store's flush), then a new session promoted before that snapshot arrives. The new
 * session's first STATUS still says at least the pushed event (promotion never lowers what was
 * pushed), so the keypad keeps its events: it never reads a log started again and empties its
 * Activity list. */
static void test_events_pushed_before_a_new_session_are_kept(void)
{
    kp_t *a = kp_new();
    pair_kp(a);
    EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_MASTER));
    TEST_ASSERT_TRUE(run_until(2000u, c_synced, a));
    run_ms(500u);
    const uint32_t old_sid = a->k.live.sid;
    /* The disarm's events go to the keypad; the snapshot that carries them waits. */
    const unsigned n0 = R.n_ev;
    R.snap_held = true;
    EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_MASTER));
    TEST_ASSERT_TRUE(R.n_ev > n0);
    const uint32_t pushed = R.ev[R.n_ev - 1u].seq;
    TEST_ASSERT_TRUE(R.snap.last_event_seq < pushed); /* the master's snapshot lags */
    run_ms(20u);
    TEST_ASSERT_EQUAL_UINT32(pushed, a->k.events[a->k.ev_count - 1u].seq);
    /* A new session now (the keypad's seq reached 2^30): HELLO, WELCOME, promotion, STATUS. */
    ring_watch_t w = {a, a->k.ev_count, false};
    a->k.live.tx_seq = PLINK_SEQ_REKEY - 1u;
    plink_keypad_addr_changed(&a->k, know(a));
    (void)run_until(300u, c_ring_dropped, &w);
    TEST_ASSERT_NOT_EQUAL(old_sid, a->k.live.sid);
    TEST_ASSERT_EQUAL_UINT32(a->k.live.sid, a->k.items_sid); /* its STATUS was applied */
    TEST_ASSERT_TRUE(a->k.view.snap.last_event_seq >= pushed);
    TEST_ASSERT_FALSE_MESSAGE(w.dropped, "the keypad dropped its events");
    R.snap_held = false;
    pump(mnow());
    TEST_ASSERT_TRUE(run_until(2000u, c_synced, a));
    check_events(a);
}

/* docs/features/11 "Event record": a keypad that pairs with a master holding more than 100
 * events ends with the newest 50 - the master answers an EV_REQ with up to 50, in as many EVENTS
 * as the transport needs - and they are the master's. */
static void test_a_new_keypad_gets_the_newest_50_events(void)
{
    for (unsigned i = 0; i < 60u; i++) {
        EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_MASTER));
        EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_MASTER));
    }
    TEST_ASSERT_TRUE(R.n_ev > 100u);
    kp_t *a = kp_new();
    pair_kp(a);
    run_ms(3000u);
    TEST_ASSERT_EQUAL_size_t(PLINK_EVENTS_KEPT, a->k.ev_count);
    check_events(a);
}

/* ---- status reaching the keypads ------------------------------------------------------------- */

typedef struct {
    unsigned total, within;
    unsigned late_first;  /* the first copy didn't leave at once */
    unsigned few_copies;  /* fewer than three copies in the second from the first */
    unsigned unexplained; /* shown later than the first copy that got through */
    unsigned missed;      /* no copy got through while it was watched */
    uint64_t worst;
} lat_t;

/* n changes of partition 1's state at the master's own touchscreen, watched on every keypad:
 * how long each took to show, whether the master sent it at once and again at +250 ms and +1 s
 * (LINK-006), and whether the keypad showed it as soon as the first copy that got through
 * arrived (the network's delay and the 5 ms the watch steps by). */
static void watch_changes(unsigned n, lat_t *out)
{
    memset(out, 0, sizeof *out);
    for (unsigned i = 0; i < n; i++) {
        const bool arm = R.snap.part[0].state == ALARM_STATE_DISARMED;
        const uint8_t want = (uint8_t)(arm ? ALARM_STATE_EXIT_DELAY : ALARM_STATE_DISARMED);
        R.net.log_count = 0u;
        const uint64_t t0 = NOW;
        EXPECT_RESULT(ALARM_OK, master_cmd(arm ? ALARM_CMD_ARM : ALARM_CMD_DISARM, 1u,
                                           ALARM_LEVEL_STAY, CODE_MASTER));
        uint64_t seen[KP_MAX];
        for (unsigned j = 0; j < R.nkp; j++) {
            seen[j] = UINT64_MAX;
        }
        while (NOW < t0 + 25000u) {
            bool all = true;
            for (unsigned j = 0; j < R.nkp; j++) {
                if (seen[j] == UINT64_MAX && shows(&R.kp[j]) == want) {
                    seen[j] = NOW;
                }
                all = all && seen[j] != UINT64_MAX;
            }
            if (all) {
                break;
            }
            run_ms(5u);
        }
        if (NOW < t0 + PLINK_STATUS_REPEAT2_MS + 100u) {
            run_ms(t0 + PLINK_STATUS_REPEAT2_MS + 100u - NOW); /* the copies of its first second */
        }
        for (unsigned j = 0; j < R.nkp; j++) {
            const uint64_t lat = seen[j] - t0;
            out->total++;
            out->within += lat <= 1000u;
            out->worst = lat > out->worst ? lat : out->worst;
            uint64_t first = UINT64_MAX, first_ok = UINT64_MAX;
            for (size_t e = 0; e < R.net.log_count; e++) {
                const plink_net_seen_t *s = &R.net.log[e];
                if (s->from == R.mnode && s->to == R.kp[j].node && s->type == PLINK_T_STATUS &&
                    s->at_ms >= t0) {
                    first = s->at_ms < first ? s->at_ms : first;
                    if (!s->lost && !s->corrupted && s->at_ms < first_ok) {
                        first_ok = s->at_ms;
                    }
                }
            }
            unsigned early = 0; /* copies in the second from the first */
            for (size_t e = 0; e < R.net.log_count; e++) {
                const plink_net_seen_t *s = &R.net.log[e];
                early += s->from == R.mnode && s->to == R.kp[j].node && s->type == PLINK_T_STATUS &&
                         s->at_ms >= first && s->at_ms <= first + PLINK_STATUS_REPEAT2_MS;
            }
            out->late_first += first > t0 + PLINK_STATUS_MIN_GAP_MS;
            out->few_copies += early < 3u;
            if (first_ok == UINT64_MAX || first_ok > t0 + 24000u) {
                out->missed++; /* no copy got through while it was watched (25 s) */
            } else {
                out->unexplained += seen[j] > first_ok + R.net.delay_ms + R.net.jitter_ms + 5u;
            }
        }
        run_ms(500u);
    }
}

/* LINK-024 (its protocol part), LINK-006, LINK-026: a change of state at the master shows on
 * every keypad within a second on a working network, every time. With 20 % loss and up to 150 ms
 * of jitter the master still sends each change at once (no sooner than 100 ms after the STATUS
 * before it) and again 250 ms and 1 s later, and a keypad shows it as soon as the first copy that
 * got through arrives: a change misses the second only when both early copies are lost (4 % at
 * 20 %), and very rarely no copy at all gets through for the 25 s it is watched. */
static void test_a_change_shows_on_every_keypad_within_a_second(void)
{
    kp_t *a = kp_new();
    pair_kp(a);
    kp_t *b = kp_new();
    pair_kp(b);
    R.net.jitter_ms = 30u;
    lat_t l;
    watch_changes(10u, &l);
    TEST_ASSERT_EQUAL_UINT(20u, l.total);
    TEST_ASSERT_EQUAL_UINT(20u, l.within);
    TEST_ASSERT_EQUAL_UINT(0u, l.late_first);
    TEST_ASSERT_EQUAL_UINT(0u, l.few_copies);
    TEST_ASSERT_EQUAL_UINT(0u, l.unexplained);
    TEST_ASSERT_EQUAL_UINT(0u, l.missed);
    TEST_ASSERT_TRUE(l.worst <= PLINK_STATUS_MIN_GAP_MS + 40u);

    R.net.loss_pm = 200u;
    R.net.jitter_ms = 150u;
    watch_changes(100u, &l);
    TEST_ASSERT_EQUAL_UINT(200u, l.total);
    TEST_ASSERT_EQUAL_UINT(0u, l.late_first);
    TEST_ASSERT_EQUAL_UINT(0u, l.few_copies);
    TEST_ASSERT_EQUAL_UINT(0u, l.unexplained);
    TEST_ASSERT_TRUE(l.missed <= 2u);
    TEST_ASSERT_TRUE_MESSAGE(l.within >= 180u, "fewer than 90 % of the changes within 1 s");
    R.net.loss_pm = 0u;
    TEST_ASSERT_TRUE(run_until(12000u, c_all_synced, NULL));
}

/* ---- restarts -------------------------------------------------------------------------------- */

/* LINK-015, LINK-008: the master restarts (a new plink_master_t from its stored record, a new
 * clock, the engine from its persisted state): every keypad is back in a new session, showing the
 * master as it is, within 15 s of the master's start, and none ever showed "No system
 * communication" meanwhile. LINK-018: a command keyed before the keypad knows of the restart goes
 * out in its old session, the master's REKEY brings the new one, and the command's next copy runs
 * in it - once, within the command's 3 s. */
static void test_master_restart_keypads_back_within_15_s(void)
{
    kp_t *k[3];
    uint32_t old_sid[3];
    for (unsigned i = 0; i < 3u; i++) {
        k[i] = kp_new();
        pair_kp(k[i]);
    }
    kdo(k[0], PLINK_OP_COMMAND, ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_USER, 1000u, ALARM_OK);
    run_ms(70000u); /* armed stay, as the engine persists it */
    for (unsigned i = 0; i < 3u; i++) {
        old_sid[i] = k[i]->k.live.sid;
    }
    R.net.node[R.mnode].down = true; /* off for 4 s */
    run_ms(4000u);
    const uint64_t t0 = NOW;
    master_restart();
    EXPECT_STATE(&R.sim, 1u, ALARM_STATE_ARMED);
    const uint32_t req =
        kcmd(k[1], PLINK_OP_COMMAND, ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_USER);
    TEST_ASSERT_NOT_EQUAL_UINT32(0u, req);
    plink_result_t res;
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_DONE, kwait(k[1], req, PLINK_CMD_GIVE_UP_MS, &res));
    EXPECT_RESULT(ALARM_OK, (alarm_result_t)res.result);
    TEST_ASSERT_EQUAL_UINT(1u, ran_req(dev(k[1]), req));
    EXPECT_STATE(&R.sim, 1u, ALARM_STATE_DISARMED);
    TEST_ASSERT_TRUE(run_until(15000u, c_all_synced, NULL));
    TEST_ASSERT_TRUE(NOW - t0 <= 15000u);
    for (unsigned i = 0; i < 3u; i++) {
        TEST_ASSERT_NOT_EQUAL_UINT32(old_sid[i], k[i]->k.live.sid);
        TEST_ASSERT_EQUAL_UINT(0u, k[i]->lost);
        TEST_ASSERT_EQUAL_INT(ALARM_STATE_DISARMED, shows(k[i]));
    }
    TEST_ASSERT_EQUAL_UINT(0u, R.n_reach);
    /* and it carries on */
    kdo(k[2], PLINK_OP_COMMAND, ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_USER, 1000u, ALARM_OK);
    run_ms(60000u);
    TEST_ASSERT_TRUE(c_all_synced(NULL));
    TEST_ASSERT_EQUAL_UINT(0u, R.n_reach);
}

/* The caps the master holds for keypad k's live session, and k's own (0: none). */
static uint16_t m_caps_of(const kp_t *k)
{
    plink_peer_info_t info[PLINK_KEYPADS_MAX];
    const size_t n = plink_master_peers(&R.m, mnow(), info, PLINK_KEYPADS_MAX);
    for (size_t i = 0; i < n; i++) {
        if (info[i].device == dev(k)) {
            return info[i].caps;
        }
    }
    TEST_FAIL_MESSAGE("no such keypad at the master");
    return 0u;
}

static uint16_t k_caps_of(const kp_t *k)
{
    plink_keypad_info_t in;
    plink_keypad_info(&k->k, know(k), &in);
    return in.caps;
}

/* LINK-043, LINK-017: the capability bits are chosen at run time and every session holds those
 * both sides offer. A master offering FW and HOME, a keypad offering FW and PREFS: both hold FW;
 * a keypad offering nothing: nothing in common. The master restarts offering PREFS too: the
 * first keypad's new session holds FW and PREFS, the other still nothing. A bit no codec here
 * knows (0x8000, 0x4000) is never offered, so never common. */
static void test_capabilities_offered_meet_in_the_and(void)
{
    R.m_caps = PLINK_CAP_FW | PLINK_CAP_HOME | 0x8000u;
    plink_master_init(&R.m, &R.cm, &R.port, NULL, "Harbor House", "a1b2c3", R.m_caps, mnow());
    remake_blob();
    plink_master_time(&R.m, mnow(), epoch_now(), "UTC0");
    pump(mnow());
    kp_t *a = kp_add();
    a->caps = PLINK_CAP_FW | PLINK_CAP_PREFS | 0x4000u;
    kp_boot(a);
    pair_kp(a);
    kp_t *b = kp_new();
    pair_kp(b);
    TEST_ASSERT_EQUAL_HEX16(PLINK_CAP_FW, m_caps_of(a));
    TEST_ASSERT_EQUAL_HEX16(PLINK_CAP_FW, k_caps_of(a));
    TEST_ASSERT_EQUAL_HEX16(0u, m_caps_of(b));
    TEST_ASSERT_EQUAL_HEX16(0u, k_caps_of(b));

    R.m_caps = PLINK_CAPS_KNOWN;
    master_restart();
    TEST_ASSERT_TRUE(run_until(15000u, c_all_synced, NULL));
    TEST_ASSERT_EQUAL_HEX16(PLINK_CAP_FW | PLINK_CAP_PREFS, m_caps_of(a));
    TEST_ASSERT_EQUAL_HEX16(PLINK_CAP_FW | PLINK_CAP_PREFS, k_caps_of(a));
    TEST_ASSERT_EQUAL_HEX16(0u, m_caps_of(b));
    TEST_ASSERT_EQUAL_HEX16(0u, k_caps_of(b));
    /* Nothing of a capability goes yet (that comes with its code): no frame of 0x2D and up
     * either way. */
    TEST_ASSERT_EQUAL_UINT8(PLINK_LINK_ONLINE, kstate(a));
    for (unsigned t = PLINK_T_FW_INFO; t < TYPES; t++) {
        TEST_ASSERT_EQUAL_UINT(0u, R.m_got[t]);
        TEST_ASSERT_EQUAL_UINT(0u, a->got[t]);
        TEST_ASSERT_EQUAL_UINT(0u, b->got[t]);
    }
}

/* LINK-018: a keypad's command runs once though its answers are lost and it gives up; the keypad
 * restarts (a new plink_keypad_t from its stored record) within the master's 90 s result cache
 * and sends another command: its request ids start from a new random base, so the new command
 * never meets the old one's cached answer, and it runs - once. */
static void test_keypad_restart_within_the_cache_runs_each_command_once(void)
{
    kp_t *a = kp_new();
    pair_kp(a);
    const uint8_t d = dev(a);
    R.run_mode = RUN_LATER;
    R.run_delay_ms = 1200u;           /* the worker answers after the first resends */
    R.net.lose_type = PLINK_T_RESULT; /* and every answer is lost */
    R.net.log_count = 0u;
    const uint64_t t1 = NOW;
    const uint32_t r1 = kcmd(a, PLINK_OP_COMMAND, ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_USER);
    TEST_ASSERT_NOT_EQUAL_UINT32(0u, r1);
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_NO_ANSWER, kwait(a, r1, 4000u, NULL));
    TEST_ASSERT_TRUE(plink_net_count(&R.net, a->node, PLINK_T_CMD) >= 6u);
    TEST_ASSERT_EQUAL_UINT(1u, ran_req(d, r1));
    EXPECT_STATE(&R.sim, 1u, ALARM_STATE_EXIT_DELAY);
    R.net.lose_type = 0u;

    /* Off and on again, a few seconds later. */
    kp_off(a);
    run_ms(3000u);
    kp_boot(a);
    TEST_ASSERT_TRUE(run_until(15000u, c_synced, a));
    const uint32_t r2 =
        kcmd(a, PLINK_OP_COMMAND, ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_USER);
    TEST_ASSERT_NOT_EQUAL_UINT32(0u, r2);
    TEST_ASSERT_NOT_EQUAL_UINT32(r1, r2);
    TEST_ASSERT_TRUE(NOW - t1 < PLINK_RESULT_KEEP_MS);
    plink_result_t res;
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_DONE, kwait(a, r2, 3000u, &res));
    EXPECT_RESULT(ALARM_OK, (alarm_result_t)res.result);
    TEST_ASSERT_EQUAL_UINT(1u, ran_req(d, r1));
    TEST_ASSERT_EQUAL_UINT(1u, ran_req(d, r2));
    TEST_ASSERT_EQUAL_UINT(2u, ran_dev(d));
    EXPECT_STATE(&R.sim, 1u, ALARM_STATE_DISARMED);
    run_ms(100000u); /* past the cache */
    TEST_ASSERT_EQUAL_UINT(2u, ran_dev(d));
    TEST_ASSERT_TRUE(synced(a));
}

/* ---- supervision ----------------------------------------------------------------------------- */

/* LINK-008, LINK-007 both ways: the master's frames stop reaching a keypad. The keypad shows LOST
 * 30 s after its last status; the master still hears it, but its heartbeats say the status they
 * have is old, and once that has lasted the supervision time the master raises the trouble. The
 * heartbeats keep coming, but none that reports a stale status checks the keypad in (the
 * engine's check-in, wired here as on a panel, would clear the trouble): the trouble stays raised
 * in the engine while they come. Back together: the keypad is ONLINE again (no sooner than 3 s
 * after LOST), the trouble clears once both directions are fresh, and check-ins resume. */
static void test_master_to_keypad_cut_lost_at_30_s_and_trouble_by_status_age(void)
{
    kp_t *a = kp_new();
    pair_kp(a);
    const uint8_t d = dev(a);
    const uint32_t bit = 1u << (d - 1u);
    TEST_ASSERT_TRUE(plink_master_set_supervision(&R.m, mnow(), d, ONE_MINUTE_S));
    run_ms(5000u);
    R.net.cut_to = a->node;
    R.net.log_count = 0u;
    const unsigned heard0 = R.checkins[d];
    want_t w = {a, (uint8_t)PLINK_LINK_LOST};
    TEST_ASSERT_TRUE(run_until(40000u, c_state, &w));
    const uint64_t last_status = net_of(a->k.status_ms, a->off);
    TEST_ASSERT_UINT64_WITHIN(60u, last_status + PLINK_LOST_MS, a->lost_at);
    TEST_ASSERT_EQUAL_UINT(0u, edges(d, false));
    /* The master hears its heartbeats all along: down only by the status age they report. Only
     * those of the first 30 s (a fresh status) checked it in. */
    edge_t e = {d, false};
    TEST_ASSERT_TRUE(run_until(150000u, c_edge, &e));
    TEST_ASSERT_TRUE(plink_net_count(&R.net, a->node, PLINK_T_HEARTBEAT) >= 8u);
    TEST_ASSERT_TRUE(R.checkins[d] <= heard0 + 4u);
    TEST_ASSERT_EQUAL_UINT(1u, edges(d, false));
    const uint64_t down = edge_at(d, false);
    TEST_ASSERT_TRUE(down >= last_status + PLINK_STATUS_STALE_S * 1000u + ONE_MINUTE_S * 1000u);
    TEST_ASSERT_TRUE(down <=
                     last_status + PLINK_STATUS_STALE_S * 1000u + 12000u + ONE_MINUTE_S * 1000u);
    run_ms(300u);
    TEST_ASSERT_TRUE((R.snap.dev_troubles & bit) != 0u);
    plink_peer_info_t pi;
    TEST_ASSERT_EQUAL_size_t(1u, plink_master_peers(&R.m, mnow(), &pi, 1u));
    TEST_ASSERT_TRUE(pi.down);
    /* A minute more of heartbeats reporting the stale status: no check-in, the engine's trouble
     * stays raised, told once. */
    const unsigned heard1 = R.checkins[d];
    R.net.log_count = 0u;
    run_ms(60000u);
    TEST_ASSERT_TRUE(plink_net_count(&R.net, a->node, PLINK_T_HEARTBEAT) >= 5u);
    TEST_ASSERT_EQUAL_UINT(heard1, R.checkins[d]);
    TEST_ASSERT_TRUE((R.snap.dev_troubles & bit) != 0u);
    alarm_snapshot_t now_snap;
    alarm_engine_snapshot(&R.sim.eng, &now_snap);
    TEST_ASSERT_TRUE((now_snap.dev_troubles & bit) != 0u);
    TEST_ASSERT_EQUAL_UINT(1u, edges(d, false));
    TEST_ASSERT_EQUAL_UINT(0u, edges(d, true));
    /* Back together. */
    const uint64_t t_back = NOW;
    R.net.cut_to = -1;
    TEST_ASSERT_TRUE(run_until(60000u, c_synced, a));
    TEST_ASSERT_TRUE(a->online_at >= a->lost_at + PLINK_LOST_MIN_MS);
    run_ms(15000u);
    TEST_ASSERT_EQUAL_UINT(1u, edges(d, true));
    TEST_ASSERT_TRUE(edge_at(d, true) >= t_back);
    TEST_ASSERT_EQUAL_UINT32(0u, R.snap.dev_troubles & bit);
    /* (the heartbeat that brought it up cleared the trouble; the next one checks it in) */
    run_ms(PLINK_HEARTBEAT_MS + 1500u);
    TEST_ASSERT_TRUE(R.checkins[d] > heard1);
    TEST_ASSERT_EQUAL_UINT32(0u, R.snap.dev_troubles & bit);
    TEST_ASSERT_TRUE(synced(a));
}

/* LINK-007: the keypad's frames stop reaching the master. It still hears the master (ONLINE all
 * along, never LOST); the master raises the trouble once it has heard nothing for the supervision
 * time, the keypad shows its own trouble from the next STATUS, and its next heartbeat clears it. */
static void test_keypad_to_master_cut_trouble_after_the_supervision_time(void)
{
    kp_t *a = kp_new();
    pair_kp(a);
    const uint8_t d = dev(a);
    TEST_ASSERT_TRUE(plink_master_set_supervision(&R.m, mnow(), d, ONE_MINUTE_S));
    run_ms(5000u);
    const uint64_t t_cut = NOW;
    R.net.cut_from = a->node;
    edge_t e = {d, false};
    TEST_ASSERT_TRUE(run_until(80000u, c_edge, &e));
    /* the last heartbeat it heard came at most 11 s before the cut */
    TEST_ASSERT_TRUE(edge_at(d, false) >= t_cut + ONE_MINUTE_S * 1000u - 11000u);
    TEST_ASSERT_TRUE(edge_at(d, false) <= t_cut + ONE_MINUTE_S * 1000u + 100u);
    TEST_ASSERT_TRUE(run_until(1000u, c_synced, a)); /* its own trouble on its screen */
    TEST_ASSERT_TRUE((plink_keypad_snapshot(&a->k)->dev_troubles & (1u << (d - 1u))) != 0u);
    TEST_ASSERT_EQUAL_UINT(0u, a->lost);
    TEST_ASSERT_EQUAL_INT(PLINK_LINK_ONLINE, kstate(a));
    const uint64_t t_back = NOW;
    R.net.cut_from = -1;
    e.up = true;
    TEST_ASSERT_TRUE(run_until(12000u, c_edge, &e));
    TEST_ASSERT_TRUE(edge_at(d, true) >= t_back);
    TEST_ASSERT_TRUE(run_until(1000u, c_synced, a));
    TEST_ASSERT_EQUAL_UINT32(0u, plink_keypad_snapshot(&a->k)->dev_troubles & (1u << (d - 1u)));
    TEST_ASSERT_EQUAL_UINT(0u, a->lost);
}

/* LINK-007: a keypad switched off: the master raises its trouble once the supervision time has
 * passed since it last heard it; the other keypad shows that trouble and is not touched. Switched
 * on again (a new manager from its stored record) it is back, and the trouble clears once both
 * directions are fresh. */
static void test_keypad_switched_off_trouble_then_cleared_when_back(void)
{
    kp_t *a = kp_new();
    pair_kp(a);
    kp_t *b = kp_new();
    pair_kp(b);
    const uint8_t da = dev(a), db = dev(b);
    TEST_ASSERT_TRUE(plink_master_set_supervision(&R.m, mnow(), da, ONE_MINUTE_S));
    TEST_ASSERT_TRUE(plink_master_set_supervision(&R.m, mnow(), db, ONE_MINUTE_S));
    run_ms(3000u);
    const uint64_t t_off = NOW;
    kp_off(a);
    edge_t e = {da, false};
    TEST_ASSERT_TRUE(run_until(75000u, c_edge, &e));
    TEST_ASSERT_TRUE(edge_at(da, false) >= t_off + ONE_MINUTE_S * 1000u - 11000u);
    TEST_ASSERT_TRUE(edge_at(da, false) <= t_off + ONE_MINUTE_S * 1000u + 100u);
    TEST_ASSERT_EQUAL_UINT(0u, edges(db, false));
    TEST_ASSERT_TRUE(run_until(1000u, c_synced, b));
    TEST_ASSERT_TRUE((R.snap.dev_troubles & (1u << (da - 1u))) != 0u);
    TEST_ASSERT_TRUE((plink_keypad_snapshot(&b->k)->dev_troubles & (1u << (da - 1u))) != 0u);
    /* A second on, it isn't online either: it was down a moment before its last frame was a
     * period old, its first heartbeat (no status yet) having started the status-age clock. */
    run_ms(1000u);
    plink_peer_info_t pi[2];
    TEST_ASSERT_EQUAL_size_t(2u, plink_master_peers(&R.m, mnow(), pi, 2u));
    TEST_ASSERT_EQUAL_UINT8(da, pi[0].device);
    TEST_ASSERT_TRUE(pi[0].down);
    TEST_ASSERT_FALSE(pi[0].online);
    TEST_ASSERT_FALSE(pi[1].down);
    TEST_ASSERT_TRUE(pi[1].online);
    run_ms(30000u);
    const uint64_t t_on = NOW;
    kp_boot(a);
    e.up = true;
    TEST_ASSERT_TRUE(run_until(20000u, c_edge, &e));
    TEST_ASSERT_TRUE(edge_at(da, true) >= t_on);
    TEST_ASSERT_TRUE(run_until(2000u, c_all_synced, NULL));
    TEST_ASSERT_EQUAL_UINT32(0u, R.snap.dev_troubles);
    TEST_ASSERT_EQUAL_UINT(1u, edges(da, false));
    TEST_ASSERT_EQUAL_UINT(0u, edges(db, false) + edges(db, true));
}

/* LINK-007: the master's own Wi-Fi is down for 2 minutes: no keypad trouble (the network trouble
 * covers it), though every keypad was silent for longer than its supervision time. Once it is
 * back, supervision counts from then: a keypad that stays silent gets its trouble one period
 * after the network came back, and the others none. */
static void test_master_wifi_down_two_minutes_then_supervision_resumes(void)
{
    kp_t *a = kp_new();
    pair_kp(a);
    kp_t *b = kp_new();
    pair_kp(b);
    const uint8_t da = dev(a), db = dev(b);
    TEST_ASSERT_TRUE(plink_master_set_supervision(&R.m, mnow(), da, ONE_MINUTE_S));
    TEST_ASSERT_TRUE(plink_master_set_supervision(&R.m, mnow(), db, ONE_MINUTE_S));
    run_ms(5000u);
    plink_master_net(&R.m, mnow(), false);
    R.net.cut_from = R.mnode;
    R.net.cut_to = R.mnode;
    run_ms(120000u);
    TEST_ASSERT_EQUAL_UINT(0u, R.n_reach);
    TEST_ASSERT_EQUAL_INT(PLINK_LINK_LOST, kstate(a)); /* what the keypads see meanwhile */
    TEST_ASSERT_EQUAL_INT(PLINK_LINK_LOST, kstate(b));
    /* Back; b stays silent (it was switched off meanwhile). */
    kp_off(b);
    R.net.cut_from = -1;
    R.net.cut_to = -1;
    const uint64_t t_up = NOW;
    plink_master_net(&R.m, mnow(), true);
    TEST_ASSERT_TRUE(run_until(45000u, c_synced, a));
    edge_t e = {db, false};
    TEST_ASSERT_TRUE(run_until(70000u, c_edge, &e));
    TEST_ASSERT_UINT64_WITHIN(100u, t_up + ONE_MINUTE_S * 1000u, edge_at(db, false));
    run_ms(60000u);
    TEST_ASSERT_EQUAL_UINT(0u, edges(da, false));
    TEST_ASSERT_EQUAL_UINT(1u, edges(db, false));
    TEST_ASSERT_TRUE(synced(a));
}

/* ---- panic (LINK-023) ------------------------------------------------------------------------ */

/* LINK-023: a panic keyed on a keypad during a 40 s outage of its link is resent every second and
 * reaches the master when the link is back - and runs once, though several copies arrive; the
 * keypad learns the answer and stops sending it. */
static void test_panic_during_an_outage_runs_once(void)
{
    kp_t *a = kp_new();
    pair_kp(a);
    const uint8_t d = dev(a);
    R.run_mode = RUN_MIX;
    const uint64_t t0 = NOW;
    R.net.cut_from = a->node;
    R.net.cut_to = a->node;
    run_ms(5000u);
    const uint32_t req = kcmd(a, PLINK_OP_COMMAND, ALARM_CMD_PANIC, 1u, ALARM_LEVEL_NONE, "");
    const uint64_t keyed = NOW;
    TEST_ASSERT_NOT_EQUAL_UINT32(0u, req);
    TEST_ASSERT_TRUE(plink_keypad_panic_pending(&a->k));
    run_ms(t0 + 40000u - NOW);
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_PENDING, plink_keypad_command_state(&a->k, req, NULL));
    TEST_ASSERT_EQUAL_UINT(0u, ran_dev(d));
    R.net.cut_from = -1;
    R.net.cut_to = -1;
    plink_result_t res;
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_DONE, kwait(a, req, 15000u, &res));
    EXPECT_RESULT(ALARM_OK, (alarm_result_t)res.result);
    TEST_ASSERT_TRUE(NOW - keyed < PLINK_PANIC_KEEP_MS);
    TEST_ASSERT_EQUAL_UINT(1u, ran_req(d, req));
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_PANIC, R.ran[R.n_ran - 1u].type);
    TEST_ASSERT_FALSE(plink_keypad_panic_pending(&a->k));
    run_ms(70000u);
    TEST_ASSERT_EQUAL_UINT(1u, ran_dev(d));
    TEST_ASSERT_TRUE(run_until(15000u, c_synced, a));
}

/* LINK-023, LINK-008: a panic keyed while the keypad already shows "No system communication" is
 * taken (only a panic is, LINK-008) and runs once when the link is back. */
static void test_panic_keyed_while_lost_runs_once(void)
{
    kp_t *a = kp_new();
    pair_kp(a);
    const uint8_t d = dev(a);
    R.net.cut_from = a->node;
    R.net.cut_to = a->node;
    want_t w = {a, (uint8_t)PLINK_LINK_LOST};
    TEST_ASSERT_TRUE(run_until(40000u, c_state, &w));
    TEST_ASSERT_EQUAL_UINT32(
        0u, kcmd(a, PLINK_OP_COMMAND, ALARM_CMD_ARM, 1u, ALARM_LEVEL_AWAY, CODE_USER));
    const uint32_t req = kcmd(a, PLINK_OP_COMMAND, ALARM_CMD_PANIC, 1u, ALARM_LEVEL_NONE, "");
    const uint64_t keyed = NOW;
    TEST_ASSERT_NOT_EQUAL_UINT32(0u, req);
    run_ms(30000u);
    R.net.cut_from = -1;
    R.net.cut_to = -1;
    plink_result_t res;
    TEST_ASSERT_EQUAL_INT(PLINK_CMD_DONE, kwait(a, req, 25000u, &res));
    EXPECT_RESULT(ALARM_OK, (alarm_result_t)res.result);
    TEST_ASSERT_TRUE(NOW - keyed < PLINK_PANIC_KEEP_MS);
    run_ms(70000u);
    TEST_ASSERT_EQUAL_UINT(1u, ran_req(d, req));
    TEST_ASSERT_EQUAL_UINT(1u, ran_dev(d));
    TEST_ASSERT_TRUE(run_until(15000u, c_synced, a));
}

/* ---- removal and pairing again --------------------------------------------------------------- */

/* LINK-014: removing a keypad at the master clears its trouble first, stores the record without
 * it, says BYE three times over 2 s, then frees its number; the keypad erases its record and says
 * it was removed; afterwards its frames are refused and a HELLO with its old keys is answered
 * NOT_PAIRED; the removed keypad stays quiet and the other one goes on. */
static void test_removal_says_bye_and_refuses_the_keypad(void)
{
    static uint8_t hb[PLINK_FRAME_MAX];
    size_t hb_len = 0;
    kp_t *a = kp_new();
    pair_kp(a);
    kp_t *b = kp_new();
    pair_kp(b);
    const uint8_t da = dev(a), db = dev(b);
    const plink_keypad_rec_t old = a->rec;
    TEST_ASSERT_TRUE(plink_master_set_supervision(&R.m, mnow(), da, ONE_MINUTE_S));
    run_ms(11000u);
    TEST_ASSERT_TRUE(plink_net_last(&R.net, a->node, PLINK_T_HEARTBEAT, hb, sizeof hb, &hb_len));
    /* Its trouble is raised: the master stopped hearing it. */
    R.net.cut_from = a->node;
    edge_t e = {da, false};
    TEST_ASSERT_TRUE(run_until(80000u, c_edge, &e));
    R.net.log_count = 0u;
    const unsigned n0 = R.n_reach;
    TEST_ASSERT_TRUE(plink_master_remove(&R.m, mnow(), da));
    TEST_ASSERT_EQUAL_UINT(n0 + 1u, R.n_reach);
    TEST_ASSERT_EQUAL_UINT8(da, R.reach[n0].device);
    TEST_ASSERT_TRUE(R.reach[n0].up);
    TEST_ASSERT_EQUAL_UINT8(1u, R.stored.count);
    TEST_ASSERT_EQUAL_UINT8(db, R.stored.peer[0].device);
    TEST_ASSERT_EQUAL_UINT(1u, R.notes[PLINK_NOTE_REMOVED]);
    want_t w = {a, (uint8_t)PLINK_LINK_NOT_PAIRED};
    TEST_ASSERT_TRUE(run_until(1000u, c_state, &w));
    plink_keypad_info_t in;
    plink_keypad_info(&a->k, know(a), &in);
    TEST_ASSERT_EQUAL_UINT8(PLINK_UNPAIRED_REMOVED, in.unpaired_why);
    TEST_ASSERT_FALSE(a->have_rec);
    TEST_ASSERT_EQUAL_UINT(1u, a->erases);
    run_ms(3000u);
    TEST_ASSERT_EQUAL_UINT(3u, m_sent_to(a->node, PLINK_T_BYE));
    TEST_ASSERT_EQUAL_UINT32(1u << (db - 1u), plink_master_held(&R.m));
    TEST_ASSERT_EQUAL_UINT32(1u << (db - 1u), R.held);
    TEST_ASSERT_EQUAL_size_t(1u, plink_master_peers(&R.m, mnow(), NULL, 0u));
    R.net.cut_from = -1;

    /* Its frames are refused: a heartbeat of its old session, a HELLO made with its old keys. */
    const unsigned heard = R.checkins[da];
    R.net.log_count = 0u;
    plink_net_inject(&R.net, a->node, maddr(), hb, hb_len);
    run_ms(500u);
    TEST_ASSERT_EQUAL_UINT(heard, R.checkins[da]);
    TEST_ASSERT_EQUAL_UINT(0u, m_sent_to(a->node, PLINK_T_REKEY));
    plink_test_port_t tp;
    plink_test_port_init(&tp, 0x0DDBA11ull);
    const plink_crypto_t c = plink_test_port(&tp);
    plink_hs_k_t hs;
    TEST_ASSERT_TRUE(plink_hs_hello(&c, old.k_pair, old.net_id, old.device, &CAP, 0u, &hs));
    plink_net_inject(&R.net, a->node, maddr(), hs.hello, hs.hello_len);
    run_ms(500u);
    TEST_ASSERT_EQUAL_UINT(1u, m_sent_to(a->node, PLINK_T_NOT_PAIRED));
    TEST_ASSERT_EQUAL_UINT(0u, m_sent_to(a->node, PLINK_T_WELCOME));
    /* The removed keypad itself stays quiet; the other one goes on. */
    run_ms(60000u);
    TEST_ASSERT_EQUAL_UINT(0u, sent_by(a->node));
    TEST_ASSERT_EQUAL_INT(PLINK_LINK_NOT_PAIRED, kstate(a));
    TEST_ASSERT_TRUE(synced(b));
    TEST_ASSERT_EQUAL_UINT(1u, edges(da, true));
    TEST_ASSERT_EQUAL_UINT(0u, edges(db, false));
}

/* LINK-001, LINK-014: a removed keypad pairs again from scratch: it takes the lowest free number
 * (its old one) with a new pairing key and syncs, events and all. */
static void test_removed_keypad_pairs_again(void)
{
    kp_t *a = kp_new();
    pair_kp(a);
    kp_t *b = kp_new();
    pair_kp(b);
    TEST_ASSERT_EQUAL_UINT8(1u, dev(a));
    TEST_ASSERT_EQUAL_UINT8(2u, dev(b));
    uint8_t old_key[PLINK_KEY_LEN];
    memcpy(old_key, a->rec.k_pair, sizeof old_key);
    TEST_ASSERT_TRUE(plink_master_remove(&R.m, mnow(), 1u));
    run_ms(3000u);
    TEST_ASSERT_EQUAL_INT(PLINK_LINK_NOT_PAIRED, kstate(a));
    TEST_ASSERT_EQUAL_UINT32(0x02u, plink_master_held(&R.m));
    pair_kp(a);
    TEST_ASSERT_EQUAL_UINT8(1u, dev(a));
    TEST_ASSERT_FALSE(memcmp(old_key, a->rec.k_pair, PLINK_KEY_LEN) == 0);
    TEST_ASSERT_EQUAL_UINT32(0x03u, plink_master_held(&R.m));
    TEST_ASSERT_EQUAL_UINT8(2u, R.stored.count);
    plink_keypad_info_t in;
    plink_keypad_info(&a->k, know(a), &in);
    TEST_ASSERT_EQUAL_UINT8(PLINK_UNPAIRED_NEVER, in.unpaired_why);
    TEST_ASSERT_TRUE(c_all_synced(NULL));
    check_events(a);
}

/* ---- eight keypads --------------------------------------------------------------------------- */

static bool all_config_seq(unsigned seq)
{
    for (unsigned i = 0; i < R.nkp; i++) {
        if (plink_keypad_config_seq(&R.kp[i].k) != seq) {
            return false;
        }
    }
    return true;
}

/* LINK-001, LINK-005, LINK-006, LINK-008: eight keypads take numbers 1 to 8, and then pairing
 * can't open; all eight start at once on a network losing 5 % and pull the configuration
 * together (the master's request limits and handshake budget hold, every page arrives): all
 * ONLINE with the master's copy. A new configuration at the master reaches all eight. */
static void test_eight_keypads_pull_the_configuration_through_loss(void)
{
    for (unsigned i = 0; i < KP_MAX; i++) {
        kp_t *k = kp_new();
        pair_kp(k);
        TEST_ASSERT_EQUAL_UINT8(i + 1u, dev(k));
    }
    TEST_ASSERT_EQUAL_UINT32(0xFFu, plink_master_held(&R.m));
    TEST_ASSERT_FALSE(plink_master_pair_open(&R.m, mnow()));
    TEST_ASSERT_TRUE(R.blob_len > PLINK_PAYLOAD_MAX - PLINK_CFG_PAGE_HDR); /* several pages */
    R.net.loss_pm = 50u;
    R.net.jitter_ms = 20u;
    for (unsigned i = 0; i < KP_MAX; i++) {
        kp_off(&R.kp[i]);
    }
    run_ms(2000u);
    for (unsigned i = 0; i < KP_MAX; i++) {
        kp_boot(&R.kp[i]);
    }
    TEST_ASSERT_TRUE(run_until(60000u, c_all_synced, NULL));
    TEST_ASSERT_TRUE(all_config_seq(1u));
    for (unsigned i = 0; i < KP_MAX; i++) {
        check_events(&R.kp[i]);
    }

    /* A zone renamed at the master: every keypad pulls the new copy. */
    CFG_TMP = R.sim.cfg;
    snprintf(CFG_TMP.zones[Z_WINDOW - 1u].name, sizeof CFG_TMP.zones[0].name, "Bay Window");
    sync_engine(mnow());
    EXPECT_RESULT(ALARM_OK, alarm_engine_set_config(&R.sim.eng, &CFG_TMP, R.sim.now));
    R.sim.cfg = CFG_TMP;
    remake_blob();
    pump(mnow());
    TEST_ASSERT_TRUE(run_until(30000u, c_all_synced, NULL));
    TEST_ASSERT_TRUE(all_config_seq(2u));
    for (unsigned i = 0; i < KP_MAX; i++) {
        TEST_ASSERT_EQUAL_STRING("Bay Window",
                                 plink_keypad_config(&R.kp[i].k)->zones[Z_WINDOW - 1u].name);
    }
    TEST_ASSERT_EQUAL_UINT(0u, R.n_reach);
}

/* ---- replays --------------------------------------------------------------------------------- */

/* LINK-003, LINK-015, LINK-021: captured frames replayed are refused, also after either side
 * restarts. In the same session a command replayed doesn't run again. The master restarted: the
 * old session's command doesn't run, an old HELLO gets nowhere, and once the keypad is back
 * neither drops its new session. The keypad restarted: the old session's STATUS and an old
 * WELCOME don't move it out of CONNECTING; once it is back a STATUS replayed changes nothing. */
static void test_replays_after_either_side_restarts_are_refused(void)
{
    static uint8_t hello[PLINK_FRAME_MAX], welcome[PLINK_FRAME_MAX], cmd[PLINK_FRAME_MAX],
        st[PLINK_FRAME_MAX], st2[PLINK_FRAME_MAX];
    size_t hello_len = 0, welcome_len = 0, cmd_len = 0, st_len = 0, st2_len = 0;
    kp_t *a = kp_new();
    pair_kp(a);
    const uint8_t d = dev(a);
    TEST_ASSERT_TRUE(
        plink_net_last(&R.net, a->node, PLINK_T_HELLO, hello, sizeof hello, &hello_len));
    TEST_ASSERT_TRUE(
        plink_net_last(&R.net, R.mnode, PLINK_T_WELCOME, welcome, sizeof welcome, &welcome_len));
    kdo(a, PLINK_OP_COMMAND, ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_USER, 1000u, ALARM_OK);
    TEST_ASSERT_TRUE(plink_net_last(&R.net, a->node, PLINK_T_CMD, cmd, sizeof cmd, &cmd_len));
    EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_USER));
    run_ms(2000u);
    /* The same session: the window refuses it. */
    plink_net_inject(&R.net, a->node, maddr(), cmd, cmd_len);
    run_ms(500u);
    TEST_ASSERT_EQUAL_UINT(1u, ran_dev(d));
    EXPECT_STATE(&R.sim, 1u, ALARM_STATE_DISARMED);

    /* The master restarts. */
    R.net.node[R.mnode].down = true;
    run_ms(3000u);
    master_restart();
    plink_net_inject(&R.net, a->node, maddr(), cmd, cmd_len);
    plink_net_inject(&R.net, a->node, maddr(), hello, hello_len);
    run_ms(300u);
    TEST_ASSERT_EQUAL_UINT(1u, ran_dev(d));
    TEST_ASSERT_TRUE(run_until(20000u, c_synced, a));
    const uint32_t sid = R.m.peer[d - 1u].live.sid;
    plink_net_inject(&R.net, a->node, maddr(), cmd, cmd_len);
    plink_net_inject(&R.net, a->node, maddr(), hello, hello_len);
    run_ms(2000u);
    TEST_ASSERT_EQUAL_UINT(1u, ran_dev(d));
    TEST_ASSERT_EQUAL_UINT32(sid, R.m.peer[d - 1u].live.sid);
    TEST_ASSERT_TRUE(synced(a));
    EXPECT_STATE(&R.sim, 1u, ALARM_STATE_DISARMED);

    /* The keypad restarts; its own HELLOs don't get out for a while. */
    TEST_ASSERT_TRUE(plink_net_last(&R.net, R.mnode, PLINK_T_STATUS, st, sizeof st, &st_len));
    kp_off(a);
    run_ms(1000u);
    R.net.cut_from = a->node;
    kp_boot(a);
    plink_net_inject(&R.net, R.mnode, plink_net_addr(&R.net, a->node), st, st_len);
    plink_net_inject(&R.net, R.mnode, plink_net_addr(&R.net, a->node), welcome, welcome_len);
    run_ms(1500u);
    TEST_ASSERT_EQUAL_INT(PLINK_LINK_CONNECTING, kstate(a));
    TEST_ASSERT_EQUAL_UINT32(0u, plink_keypad_seq(&a->k));
    TEST_ASSERT_FALSE(a->k.live.valid);
    R.net.cut_from = -1;
    TEST_ASSERT_TRUE(run_until(20000u, c_synced, a));
    /* Back: the old session's STATUS and this session's last one, replayed, change nothing. */
    TEST_ASSERT_TRUE(plink_net_last(&R.net, R.mnode, PLINK_T_STATUS, st2, sizeof st2, &st2_len));
    R.net.cut_from = R.mnode; /* nothing genuine meanwhile ... */
    run_ms(50u);              /* ... once what is under way has landed */
    const uint32_t seq = plink_keypad_seq(&a->k);
    plink_net_inject(&R.net, R.mnode, plink_net_addr(&R.net, a->node), st, st_len);
    plink_net_inject(&R.net, R.mnode, plink_net_addr(&R.net, a->node), st2, st2_len);
    run_ms(300u);
    TEST_ASSERT_EQUAL_UINT32(seq, plink_keypad_seq(&a->k));
    R.net.cut_from = -1;
    TEST_ASSERT_TRUE(run_until(12000u, c_synced, a));
    TEST_ASSERT_EQUAL_UINT(1u, ran_dev(d));
}

/* ---- addresses ------------------------------------------------------------------------------- */

typedef struct {
    const kp_t *k;
    const plink_addr_t *master;
} at_t;

/* The keypad shows the master and sends to it where it is now. */
static bool c_synced_at(void *ctx)
{
    const at_t *w = ctx;
    plink_keypad_info_t in;
    plink_keypad_info(&w->k->k, know(w->k), &in);
    return synced(w->k) && plink_addr_equal(&in.master, w->master);
}

/* LINK-021 (its protocol part): addresses move with authenticated frames, in the same session.
 * The keypad's address changes (DHCP): its heartbeat at once moves the master's idea of it and
 * the status follows it there. The master's address changes: its next STATUS, authenticated,
 * moves the keypad's idea of it, before the keypad would start a handshake or show "No system
 * communication". */
static void test_addresses_move_with_authenticated_frames(void)
{
    kp_t *a = kp_new();
    pair_kp(a);
    const uint8_t d = dev(a);
    const uint32_t sid = a->k.live.sid;
    const uint8_t kip[4] = {192u, 0u, 2u, 77u};
    R.net.node[a->node].addr = plink_addr_udp(kip, (uint16_t)PLINK_UDP_PORT);
    plink_keypad_addr_changed(&a->k, know(a));
    run_ms(100u);
    TEST_ASSERT_TRUE(plink_addr_equal(&R.net.node[a->node].addr, &R.m.peer[d - 1u].addr));
    EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_USER));
    want_t w = {a, (uint8_t)ALARM_STATE_EXIT_DELAY};
    TEST_ASSERT_TRUE(run_until(1000u, c_shows, &w));

    const uint8_t mip[4] = {192u, 0u, 2u, 2u};
    R.net.node[R.mnode].addr = plink_addr_udp(mip, (uint16_t)PLINK_UDP_PORT);
    at_t at = {a, maddr()};
    TEST_ASSERT_TRUE(run_until(PLINK_STATUS_EVERY_MS + 1000u, c_synced_at, &at));
    TEST_ASSERT_EQUAL_UINT32(sid, a->k.live.sid);
    EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_USER));
    w.state = (uint8_t)ALARM_STATE_DISARMED;
    TEST_ASSERT_TRUE(run_until(1000u, c_shows, &w));
    kdo(a, PLINK_OP_COMMAND, ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_USER, 1000u, ALARM_OK);
    TEST_ASSERT_EQUAL_UINT32(sid, a->k.live.sid);
    TEST_ASSERT_EQUAL_UINT(0u, a->lost);
    TEST_ASSERT_EQUAL_UINT(0u, R.n_reach);
}

/* ---- a hostile keypad ------------------------------------------------------------------------ */

/* LINK-027, LINK-015: someone with a keypad's keys (its session taken over, sending from another
 * address) floods the master for 20 s - every 5 ms a copy of one command, a new command with a
 * wrong code, one for another partition, the three kinds of request, and every 10 ms a fresh
 * HELLO. Its commands run one at a time at about 2 a second (a burst of 5); its requests are
 * answered at the link's rate (a burst of 32, then 20 a second); its handshakes get its own share
 * of the X25519 budget (a burst of 2, then 1 a second), and no more answers than it asked for.
 * Meanwhile the other keypads' commands and status go on as before, a keypad restarting gets its
 * handshake at once, and no trouble is raised. */
static void test_hostile_keypad_is_rate_limited_and_the_others_go_on(void)
{
    kp_t *a = kp_new();
    pair_kp(a);
    kp_t *b = kp_new();
    pair_kp(b);
    kp_t *h = kp_new();
    pair_kp(h);
    const uint8_t dh = dev(h);
    R.run_mode = RUN_LATER;
    R.run_delay_ms = 300u; /* the worker */
    hostile_t *x = &R.h;
    x->s = h->k.live;
    x->rec = h->k.rec;
    x->c = &h->c;
    x->req = 0x10000000u;
    x->copy_req = 0x0FFFFFFFu;
    x->hellos = true;
    x->on = true;
    x->next = NOW;
    kp_off(h);
    const unsigned runs0 = ran_dev(dh);
    const unsigned a_ev0 = a->got[PLINK_T_EVENTS], a_zones0 = a->got[PLINK_T_ZONES];
    const uint64_t t0 = NOW;
    run_ms(5000u);
    kdo(a, PLINK_OP_COMMAND, ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_USER, 1000u, ALARM_OK);
    want_t w = {b, (uint8_t)ALARM_STATE_EXIT_DELAY};
    TEST_ASSERT_TRUE(run_until(1000u, c_shows, &w));
    run_ms(2000u);
    EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_MASTER));
    w.state = (uint8_t)ALARM_STATE_DISARMED;
    TEST_ASSERT_TRUE(run_until(1000u, c_shows, &w));
    w.k = a;
    TEST_ASSERT_TRUE(run_until(1000u, c_shows, &w));
    /* b starts again in the middle of it: its handshake goes through at once. */
    kp_off(b);
    run_ms(1000u);
    const uint64_t t_boot = NOW;
    kp_boot(b);
    TEST_ASSERT_TRUE(run_until(5000u, c_synced, b));
    TEST_ASSERT_TRUE(NOW - t_boot <= 2000u);
    if (NOW < t0 + 20000u) {
        run_ms(t0 + 20000u - NOW);
    }
    x->on = false;
    const uint64_t span = NOW - t0;
    const unsigned runs = ran_dev(dh) - runs0;
    TEST_ASSERT_TRUE_MESSAGE(runs <= 5u + (unsigned)(span / 500u) + 1u, "too many commands ran");
    TEST_ASSERT_TRUE(runs >= (unsigned)(span / 1000u)); /* it does get its share */
    /* What it was answered for its requests: the configuration's pages, and the events and zones
     * beyond those every live keypad was pushed (keypad a got the same pushes, asking nothing). */
    const unsigned answers = x->got[PLINK_T_CFG_PAGE] +
                             (x->got[PLINK_T_EVENTS] - (a->got[PLINK_T_EVENTS] - a_ev0)) +
                             (x->got[PLINK_T_ZONES] - (a->got[PLINK_T_ZONES] - a_zones0));
    TEST_ASSERT_TRUE(x->got[PLINK_T_EVENTS] >= a->got[PLINK_T_EVENTS] - a_ev0);
    TEST_ASSERT_TRUE_MESSAGE(answers <= 32u + (unsigned)(span / 50u) + 1u, "too many answers");
    TEST_ASSERT_TRUE(answers >= (unsigned)(span / 50u) - 20u);
    TEST_ASSERT_TRUE(x->got[PLINK_T_WELCOME] <= 2u + (unsigned)(span / 1000u));
    TEST_ASSERT_TRUE(x->got[PLINK_T_RESULT] <= x->cmds);
    TEST_ASSERT_TRUE(x->got[PLINK_T_RESULT] >= x->bursts); /* its copies and refusals answered */
    TEST_ASSERT_EQUAL_UINT(0u, R.n_reach);
    run_ms(2000u);
    TEST_ASSERT_TRUE(synced(a));
    TEST_ASSERT_TRUE(synced(b));
}

/* ---- the long run ---------------------------------------------------------------------------- */

/* Six hours on a damaged network - 10 % loss, 2 % duplicated, 1 % corrupted, up to 300 ms of
 * jitter (which reorders) - with the keypads arming, disarming and checking codes, the master's
 * touchscreen disarming, a window opening and closing, and one keypad switched off for 10
 * minutes. Nothing breaks: every keypad keeps a session the master knows, is LOST only when the
 * network lost every STATUS of 30 s, and ends showing the master as it is, its events and all;
 * each command is answered and runs once; the only trouble is the switched-off keypad's (raised
 * after its 5 minutes, cleared when it is back); no side ever uses a key and nonce twice
 * (LINK-003, LINK-006, LINK-007, LINK-008, LINK-015, LINK-018, LINK-026). */
static void test_six_hours_on_a_damaged_network(void)
{
    enum { N = 4 };
    kp_t *k[N];
    for (unsigned i = 0; i < N; i++) {
        k[i] = kp_new();
        pair_kp(k[i]);
    }
    damage_on();
    R.net.loss_pm = 100u;
    R.net.dup_pm = 20u;
    R.net.corrupt_pm = 10u;
    R.net.jitter_ms = 300u;
    R.run_mode = RUN_MIX;
    R.explain = true;
    R.n_reach = 0u;
    for (unsigned i = 0; i < N; i++) {
        k[i]->lost = 0u;
        k[i]->offline_ms = 0u;
        k[i]->sessions = 0u;
    }
    const uint8_t d3 = dev(k[3]);
    unsigned sent = 0, done = 0, gave_up = 0, refused = 0;
    bool window = false;
    uint64_t t_off = 0, t_on = 0;
    for (unsigned min = 0; min < 360u; min++) {
        kp_t *who = k[(min / 6u) % N];
        const bool on = !R.net.node[who->node].down;
        uint32_t req = 0;
        bool asked = false;
        switch (min % 6u) {
        case 0:
            asked = on;
            req = on ? kcmd(who, PLINK_OP_COMMAND, ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_USER)
                     : 0u;
            break;
        case 2:
            asked = on;
            req =
                on ? kcmd(who, PLINK_OP_COMMAND, ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_USER)
                   : 0u;
            break;
        case 3:
            window = !window;
            m_zone(Z_WINDOW, window);
            break;
        case 4:
            asked = on;
            req = on ? kcmd(who, PLINK_OP_AUTH, ALARM_CMD_CODE_CHECK, 1u, ALARM_LEVEL_NONE,
                            CODE_MASTER)
                     : 0u;
            break;
        case 5: (void)master_cmd(ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_MASTER); break;
        default: break;
        }
        if (asked) {
            sent += req != 0u;
            refused += req == 0u;
        }
        if (min == 120u) {
            t_off = NOW;
            kp_off(k[3]);
        }
        if (min == 130u) {
            t_on = NOW;
            kp_boot(k[3]);
        }
        for (unsigned s = 0; s < 6u; s++) {
            log_trim();
            run_ms(10000u);
        }
        if (req != 0u && !R.net.node[who->node].down) {
            const plink_cmd_state_t st = plink_keypad_command_state(&who->k, req, NULL);
            done += st == PLINK_CMD_DONE;
            gave_up += st == PLINK_CMD_NO_ANSWER;
            plink_keypad_command_release(&who->k, req);
        }
    }
    /* Quiet again: everybody shows the master as it is, damaged network or not. */
    (void)master_cmd(ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_MASTER);
    TEST_ASSERT_TRUE(run_until(120000u, c_all_synced, NULL));
    R.net.loss_pm = R.net.dup_pm = R.net.corrupt_pm = 0u;
    R.net.jitter_ms = 0u;
    TEST_ASSERT_TRUE(run_until(30000u, c_all_synced, NULL));
    /* Troubles only when expected: keypad 4, switched off for 10 minutes. */
    TEST_ASSERT_EQUAL_UINT(2u, R.n_reach);
    TEST_ASSERT_EQUAL_UINT(1u, edges(d3, false));
    TEST_ASSERT_EQUAL_UINT(1u, edges(d3, true));
    /* (its last frames may have reached the master up to 300 ms after it went off: the jitter) */
    TEST_ASSERT_TRUE(edge_at(d3, false) >= t_off + PLINK_SUPERVISION_DEFAULT_S * 1000u - 40000u);
    TEST_ASSERT_TRUE(edge_at(d3, false) <= t_off + PLINK_SUPERVISION_DEFAULT_S * 1000u + 400u);
    TEST_ASSERT_TRUE(edge_at(d3, true) >= t_on);
    TEST_ASSERT_TRUE(edge_at(d3, true) <= t_on + 120000u);
    /* Sessions keep. A keypad would show "No system communication" only when the network lost
     * every STATUS of 30 s, and keeps its session through it; it is ONLINE all but seconds of the
     * six hours. A keypad starts a handshake after 15 s without a STATUS (one lost), whatever
     * else got through, and the new session brings a STATUS at once - so three lost in a row no
     * longer make it LOST (none in 84 runs: seeds 0..20, four keypads each) - and new sessions
     * come about every two minutes, never in a storm. */
    for (unsigned i = 0; i < N; i++) {
        TEST_ASSERT_EQUAL_UINT(0u, k[i]->lost_unexplained);
        TEST_ASSERT_EQUAL_UINT(0u, k[i]->lost_session);
        TEST_ASSERT_TRUE(k[i]->lost <= 8u);
        TEST_ASSERT_TRUE(k[i]->sessions <= 360u);
    }
    for (unsigned i = 0; i < 3u; i++) {
        TEST_ASSERT_TRUE(k[i]->offline_ms <= 60000u);
    }
    /* Every keypad's events are the master's newest, none missing in between. */
    for (unsigned i = 0; i < N; i++) {
        check_events(k[i]);
    }
    /* The commands: every one taken was answered (each copy and answer may be lost). */
    TEST_ASSERT_TRUE(sent >= 150u);
    TEST_ASSERT_TRUE(refused <= 2u);
    TEST_ASSERT_TRUE(done + gave_up == sent);
    TEST_ASSERT_TRUE(gave_up <= 2u);
}

/* ---- a keypad's voice (LINK-028) ------------------------------------------------------------- */

static const char *const KIND[] = {"none",     "entry",     "arming",    "armed",
                                   "disarmed", "walk start", "walk end", "walk warning",
                                   "not ready", "walk trip", "chime",    "preview"};

static const char *kind_name(uint8_t kind)
{
    return kind < sizeof KIND / sizeof KIND[0] ? KIND[kind] : "?";
}

/* What a voice said from [from] on, for a failure's message. */
static const char *said_text(const voice_t *v, unsigned from)
{
    static char buf[1024];
    size_t n = 0u;
    buf[0] = '\0';
    for (unsigned i = from; i < v->n_said && n + 40u < sizeof buf; i++) {
        n += (size_t)snprintf(buf + n, sizeof buf - n, "%s%s/%u@%llu", i > from ? ", " : "",
                              kind_name(v->said[i].kind), (unsigned)v->said[i].zone,
                              (unsigned long long)v->said[i].at);
    }
    return buf;
}

/* Zone 1, Front Door, has approved words ("Front" "Door", against the pack's revision): its
 * chime and walk-test trips say them. */
static void voice_words(void)
{
    alarm_zone_config_t *z = &R.sim.cfg.zones[Z_FRONT - 1u];
    alarm_zone_set_word(z, 0u, 1u);
    alarm_zone_set_word(z, 1u, 109u);
    R.sim.cfg.opt.voice_revision_counter = 20u;
    z->voice_binding_revision = 11u;
    z->voice_approved_revision = 11u;
    z->voice_pack_revision = V_PACK_REV;
    remake_blob();
}

/* Each of keypad k's utterances from its [from_k] on is one of the master's from [from_r] on, in
 * the same order (each matched once): the same kind and zone, decided at most late_ms after the
 * master's - and no sooner, but for the tone a keypad never got (an acknowledgement lost on the
 * way: its words don't wait for it). Returns how many it said; s_latest is the most any was
 * late. */
static uint64_t s_latest;

static unsigned said_as_master(const kp_t *k, unsigned from_k, unsigned from_r, uint64_t late_ms)
{
    unsigned j = from_r;
    unsigned said = 0u;
    s_latest = 0u;
    for (unsigned i = from_k; i < k->v.n_said; i++) {
        const said_t *s = &k->v.said[i];
        /* the master's utterance of that kind nearest in time (one the keypad missed may come
         * before it), after the last one matched */
        unsigned m = R.mv.n_said;
        for (unsigned c = j; c < R.mv.n_said && R.mv.said[c].at <= s->at + V_TONE_MS; c++) {
            const said_t *r = &R.mv.said[c];
            if (r->kind == s->kind && r->zone == s->zone && s->at <= r->at + late_ms) {
                m = c; /* each later one is nearer */
            }
        }
        if (m == R.mv.n_said) {
            static char msg[2300];
            snprintf(msg, sizeof msg,
                     "%s said %s at %llu, which the master didn't then; it said: %s", k->name,
                     kind_name(s->kind), (unsigned long long)s->at, said_text(&R.mv, from_r));
            TEST_FAIL_MESSAGE(msg);
        }
        if (s->at > R.mv.said[m].at && s->at - R.mv.said[m].at > s_latest) {
            s_latest = s->at - R.mv.said[m].at;
            if (getenv("PLINK_VOICE_SHOW") != NULL && s_latest > 1000u) {
                printf("late: %s %s at %llu, the master's at %llu\n", k->name, kind_name(s->kind),
                       (unsigned long long)s->at, (unsigned long long)R.mv.said[m].at);
            }
        }
        j = m + 1u;
        said++;
    }
    return said;
}

/* The same utterances as the master's from [from_r] on, each within `late_ms`. */
static void said_same(const kp_t *k, unsigned from_k, unsigned from_r, uint64_t late_ms)
{
    static char msg[2300];
    snprintf(msg, sizeof msg, "the master said: %s", said_text(&R.mv, from_r));
    const unsigned n = said_as_master(k, from_k, from_r, late_ms);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(R.mv.n_said - from_r, n, msg);
}

/* What a voice said from [from] on is exactly these kinds. */
static void said_kinds(const voice_t *v, unsigned from, const uint8_t *kinds, unsigned n)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(n, v->n_said - from, said_text(v, from));
    for (unsigned i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(kind_name(kinds[i]), kind_name(v->said[from + i].kind),
                                         said_text(v, from));
    }
}

/* LINK-028 (D05 on a keypad): a keypad's voice, fed from the link, decides what the master's
 * decides, each within a second: Arming after the arm's acknowledgement, Armed at the exit's end,
 * Entry, Disarmed, Not ready after its error tone, a chime's approved words, and a walk test's
 * start (after the bell's test pulse), a trip's words and its end. Its pairing and the events it
 * pulled then said nothing. */
static void test_a_keypad_says_what_the_master_says(void)
{
    voice_words();
    kp_t *a = kp_new();
    kv_start(a);
    pair_kp(a);
    ref_start(1u);
    run_ms(2000u);
    TEST_ASSERT_EQUAL_UINT(0u, a->v.n_said);
    TEST_ASSERT_EQUAL_UINT(1u, a->v.s.baselines);
    TEST_ASSERT_EQUAL_UINT(0u, a->v.s.pauses);
    /* armed away at the keypad, out through the front door */
    kdo(a, PLINK_OP_COMMAND, ALARM_CMD_ARM, 1u, ALARM_LEVEL_AWAY, CODE_USER, 1000u, ALARM_OK);
    run_ms(5000u);
    m_zone(Z_FRONT, true);
    run_ms(3000u);
    m_zone(Z_FRONT, false);
    want_t w = {a, (uint8_t)ALARM_STATE_ARMED};
    TEST_ASSERT_TRUE(run_until(65000u, c_shows, &w));
    run_ms(2000u);
    /* someone comes in, and disarms at the keypad */
    m_zone(Z_FRONT, true);
    run_ms(3000u);
    m_zone(Z_FRONT, false);
    kdo(a, PLINK_OP_COMMAND, ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_USER, 1000u, ALARM_OK);
    run_ms(2000u);
    /* a window open: not ready */
    m_zone(Z_WINDOW, true);
    run_ms(1000u);
    kdo(a, PLINK_OP_COMMAND, ALARM_CMD_ARM, 1u, ALARM_LEVEL_AWAY, CODE_USER, 1000u,
        ALARM_ERR_NOT_READY);
    run_ms(2000u);
    m_zone(Z_WINDOW, false);
    run_ms(1000u);
    /* the chime on: the front door says its words */
    EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_CHIME, 1u, ALARM_LEVEL_NONE, CODE_USER));
    run_ms(1000u);
    m_zone(Z_FRONT, true);
    run_ms(2000u);
    m_zone(Z_FRONT, false);
    run_ms(2000u);
    /* a walk test: started, the front door tripped, ended */
    EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_WALK_TEST, 1u, ALARM_LEVEL_NONE, CODE_MASTER));
    run_ms(4000u);
    m_zone(Z_FRONT, true);
    run_ms(2000u);
    m_zone(Z_FRONT, false);
    run_ms(2000u);
    EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_WALK_TEST, 1u, ALARM_LEVEL_NONE, CODE_MASTER));
    run_ms(3000u);
    static const uint8_t KINDS[] = {ANN_KIND_ARMING,    ANN_KIND_ARMED,     ANN_KIND_ENTRY,
                                    ANN_KIND_DISARMED,  ANN_KIND_NOT_READY, ANN_KIND_CHIME,
                                    ANN_KIND_WALK_START, ANN_KIND_WALK_TRIP, ANN_KIND_WALK_END};
    said_kinds(&R.mv, 0u, KINDS, sizeof KINDS);
    TEST_ASSERT_EQUAL_UINT16(Z_FRONT, R.mv.said[5].zone);
    TEST_ASSERT_EQUAL_UINT16(Z_FRONT, R.mv.said[7].zone);
    said_same(a, 0u, 0u, 1000u);
    TEST_ASSERT_EQUAL_UINT(1u, a->v.s.baselines);
    TEST_ASSERT_TRUE(run_until(2000u, c_synced, a));
}

/* LINK-028: nothing older than a keypad's last baseline is said - after it restarts in an exit
 * time (not the arming before it; Armed at its end, as the master says it: a baseline in an exit
 * time joins its cycle), after the master restarts (its log starts again), after a cut longer
 * than 30 s (LOST: the voice paused; the catch-up and the backfill after it) - and nothing late
 * after a cut too short for LOST: neither the arm made in it (its events come in only with the
 * pull, long after the master stamped them) nor Armed when the cut spans the end of an exit time
 * it heard begin (the STATUS that brings it carries a master snapshot already old). Each time it
 * speaks again with the next change. */
static void test_a_keypad_says_nothing_old(void)
{
    voice_words();
    kp_t *a = kp_new();
    kv_start(a);
    pair_kp(a);
    ref_start(1u);
    run_ms(2000u);

    /* a keypad restart in the exit time */
    EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_ARM, 1u, ALARM_LEVEL_AWAY, CODE_USER));
    run_ms(2000u);
    static const uint8_t ARMING[] = {ANN_KIND_ARMING};
    said_kinds(&a->v, 0u, ARMING, 1u);
    unsigned k0 = a->v.n_said, r0 = R.mv.n_said;
    kp_off(a);
    run_ms(3000u);
    kp_boot(a);
    TEST_ASSERT_TRUE(run_until(15000u, c_synced, a));
    TEST_ASSERT_EQUAL_UINT(1u, a->v.s.baselines);
    want_t w = {a, (uint8_t)ALARM_STATE_ARMED};
    TEST_ASSERT_TRUE(run_until(65000u, c_shows, &w));
    run_ms(2000u);
    static const uint8_t ARMED[] = {ANN_KIND_ARMED};
    said_kinds(&R.mv, r0, ARMED, 1u);
    said_same(a, k0, r0, 1000u);
    k0 = a->v.n_said;
    r0 = R.mv.n_said;
    kdo(a, PLINK_OP_COMMAND, ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_USER, 1000u, ALARM_OK);
    run_ms(2000u);
    static const uint8_t DISARMED[] = {ANN_KIND_DISARMED};
    said_kinds(&R.mv, r0, DISARMED, 1u);
    said_same(a, k0, r0, 1000u);

    /* a master restart: the master's voice starts again too */
    k0 = a->v.n_said;
    R.net.node[R.mnode].down = true;
    run_ms(4000u);
    master_restart();
    TEST_ASSERT_TRUE(run_until(15000u, c_synced, a));
    run_ms(2000u);
    said_kinds(&a->v, k0, NULL, 0u);
    r0 = R.mv.n_said;
    kdo(a, PLINK_OP_COMMAND, ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_USER, 1000u, ALARM_OK);
    run_ms(2000u);
    kdo(a, PLINK_OP_COMMAND, ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_USER, 1000u, ALARM_OK);
    run_ms(2000u);
    static const uint8_t ARM_DISARM[] = {ANN_KIND_ARMING, ANN_KIND_DISARMED};
    said_kinds(&R.mv, r0, ARM_DISARM, 2u);
    said_same(a, k0, r0, 1000u);

    /* a cut longer than 30 s: LOST, the voice paused; back, it says nothing of what it missed */
    k0 = a->v.n_said;
    r0 = R.mv.n_said;
    const unsigned pauses = a->v.s.pauses, baselines = a->v.s.baselines;
    R.net.cut_to = a->node;
    w.state = (uint8_t)PLINK_LINK_LOST;
    TEST_ASSERT_TRUE(run_until(40000u, c_state, &w));
    TEST_ASSERT_EQUAL_UINT(pauses + 1u, a->v.s.pauses);
    EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_USER));
    run_ms(2000u);
    EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_USER));
    run_ms(2000u);
    R.net.cut_to = -1;
    TEST_ASSERT_TRUE(run_until(30000u, c_synced, a));
    run_ms(3000u);
    said_kinds(&R.mv, r0, ARM_DISARM, 2u);
    said_kinds(&a->v, k0, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT(baselines + 1u, a->v.s.baselines);

    /* a cut of 20 s: never LOST; the arm made in it comes in long after, and isn't said - nor
     * Armed at the end of an exit time it never heard begin */
    k0 = a->v.n_said;
    r0 = R.mv.n_said;
    const unsigned lost = a->lost;
    R.net.cut_to = a->node;
    run_ms(2000u);
    EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_USER));
    run_ms(18000u);
    R.net.cut_to = -1;
    TEST_ASSERT_EQUAL_UINT(lost, a->lost); /* never LOST */
    w.state = (uint8_t)ALARM_STATE_ARMED;
    TEST_ASSERT_TRUE(run_until(65000u, c_shows, &w));
    run_ms(2000u);
    said_kinds(&R.mv, r0, (const uint8_t[]){ANN_KIND_ARMING, ANN_KIND_ARMED}, 2u);
    /* at most Armed at the end, in time with the master's (the late STATUS was a baseline in the
     * exit time, which joins its cycle) */
    TEST_ASSERT_TRUE(said_as_master(a, k0, r0, 1000u) <= 1u);
    for (unsigned i = k0; i < a->v.n_said; i++) {
        TEST_ASSERT_EQUAL_STRING(kind_name(ANN_KIND_ARMED), kind_name(a->v.said[i].kind));
    }
    TEST_ASSERT_TRUE(a->v.s.f.stale + a->v.s.f.late >= 1u);
    /* and with the next change it speaks */
    k0 = a->v.n_said;
    r0 = R.mv.n_said;
    kdo(a, PLINK_OP_COMMAND, ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_USER, 1000u, ALARM_OK);
    run_ms(2000u);
    said_kinds(&R.mv, r0, DISARMED, 1u);
    said_same(a, k0, r0, 1000u);

    /* a cut of 20 s across the end of an exit time it heard begin: Armed comes in only with a
     * STATUS long after the master said it, and isn't said */
    k0 = a->v.n_said;
    r0 = R.mv.n_said;
    kdo(a, PLINK_OP_COMMAND, ALARM_CMD_ARM, 1u, ALARM_LEVEL_AWAY, CODE_USER, 1000u, ALARM_OK);
    run_ms(2000u);
    said_kinds(&a->v, k0, ARMING, 1u);
    const unsigned late = a->v.s.f.late;
    run_ms(50000u);
    R.net.cut_to = a->node;
    run_ms(20000u);
    R.net.cut_to = -1;
    TEST_ASSERT_EQUAL_UINT(lost, a->lost); /* never LOST */
    TEST_ASSERT_TRUE(run_until(15000u, c_synced, a));
    run_ms(2000u);
    said_kinds(&R.mv, r0, (const uint8_t[]){ANN_KIND_ARMING, ANN_KIND_ARMED}, 2u);
    said_kinds(&a->v, k0, ARMING, 1u);
    TEST_ASSERT_EQUAL_UINT(late + 1u, a->v.s.f.late);
    k0 = a->v.n_said;
    r0 = R.mv.n_said;
    kdo(a, PLINK_OP_COMMAND, ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_USER, 1000u, ALARM_OK);
    run_ms(2000u);
    said_kinds(&R.mv, r0, DISARMED, 1u);
    said_same(a, k0, r0, 1000u);
}

/* LINK-028, D05: a keypad speaks for its home partition: moved from 1 to 2 at the master, it
 * takes a baseline of partition 2 and from then on says partition 2's arming and disarming, as a
 * voice of partition 2 fed from the engine does, and nothing of partition 1's. */
static void test_a_keypad_moved_home_speaks_for_its_new_partition(void)
{
    two_partitions();
    kp_t *a = kp_new();
    kv_start(a);
    pair_kp(a);
    ref_start(2u);
    run_ms(2000u);
    /* partition 1's keypad says partition 1's arming; partition 2's voice says nothing of it */
    EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_USER));
    run_ms(2000u);
    EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_USER));
    run_ms(2000u);
    static const uint8_t ARM_DISARM[] = {ANN_KIND_ARMING, ANN_KIND_DISARMED};
    said_kinds(&a->v, 0u, ARM_DISARM, 2u);
    said_kinds(&R.mv, 0u, NULL, 0u);
    /* moved */
    const unsigned baselines = a->v.s.baselines;
    TEST_ASSERT_TRUE(plink_master_set_home(&R.m, mnow(), dev(a), 2u));
    TEST_ASSERT_TRUE(run_until(2000u, c_synced, a));
    run_ms(500u);
    TEST_ASSERT_EQUAL_UINT(baselines + 1u, a->v.s.baselines);
    TEST_ASSERT_EQUAL_UINT8(2u, a->v.s.a.keypad);
    const unsigned k0 = a->v.n_said;
    EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_USER));
    run_ms(2000u);
    kdo(a, PLINK_OP_COMMAND, ALARM_CMD_ARM, 2u, ALARM_LEVEL_STAY, CODE_P2, 1000u, ALARM_OK);
    run_ms(2000u);
    kdo(a, PLINK_OP_COMMAND, ALARM_CMD_DISARM, 2u, ALARM_LEVEL_NONE, CODE_P2, 1000u, ALARM_OK);
    run_ms(2000u);
    EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_USER));
    run_ms(2000u);
    said_kinds(&R.mv, 0u, ARM_DISARM, 2u);
    said_same(a, k0, 0u, 1000u);
}

/* LINK-028 under 10 % loss, duplicates and up to 150 ms of jitter, on two keypads: each says only
 * what the master decided, in order, each once, and never late - within a second of the stale
 * guard's 5 s (seen: about 1 s at worst, the STATUS repeated at +1 s); a lost frame costs an
 * announcement, never a wrong one, and most still speak. */
static void test_under_loss_a_keypad_never_says_more_than_the_master(void)
{
    voice_words();
    kp_t *k[2];
    for (unsigned i = 0; i < 2u; i++) {
        k[i] = kp_new();
        kv_start(k[i]);
        pair_kp(k[i]);
    }
    ref_start(1u);
    EXPECT_RESULT(ALARM_OK, master_cmd(ALARM_CMD_CHIME, 1u, ALARM_LEVEL_NONE, CODE_USER));
    run_ms(2000u);
    const unsigned r0 = R.mv.n_said;
    unsigned k0[2];
    for (unsigned i = 0; i < 2u; i++) {
        k0[i] = k[i]->v.n_said;
    }
    R.net.loss_pm = 100u;
    R.net.dup_pm = 20u;
    R.net.jitter_ms = 150u;
    for (unsigned round = 0; round < 32u; round++) {
        switch (round % 4u) {
        case 0u: /* armed and disarmed at the master's own screen */
            (void)master_cmd(ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_USER);
            run_ms(3000u);
            (void)master_cmd(ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_USER);
            run_ms(3000u);
            break;
        case 1u: /* the chime */
            m_zone(Z_FRONT, true);
            run_ms(2000u);
            m_zone(Z_FRONT, false);
            run_ms(2000u);
            break;
        case 2u: /* not ready */
            m_zone(Z_WINDOW, true);
            run_ms(1000u);
            (void)master_cmd(ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_USER);
            run_ms(2000u);
            m_zone(Z_WINDOW, false);
            run_ms(1000u);
            break;
        default: { /* armed at a keypad, disarmed at the master */
            kp_t *who = k[(round / 4u) % 2u];
            const uint32_t req =
                kcmd(who, PLINK_OP_COMMAND, ALARM_CMD_ARM, 1u, ALARM_LEVEL_STAY, CODE_USER);
            if (req != 0u) {
                (void)kwait(who, req, 3000u, NULL);
                plink_keypad_command_release(&who->k, req);
            }
            run_ms(3000u);
            (void)master_cmd(ALARM_CMD_DISARM, 1u, ALARM_LEVEL_NONE, CODE_USER);
            run_ms(3000u);
            break;
        }
        }
    }
    R.net.loss_pm = R.net.dup_pm = 0u;
    R.net.jitter_ms = 0u;
    run_ms(5000u);
    /* 8 rounds each: Arming and Disarmed twice, a chime's words, Not ready */
    const unsigned master = R.mv.n_said - r0;
    TEST_ASSERT_TRUE_MESSAGE(master >= 44u, said_text(&R.mv, r0));
    for (unsigned i = 0; i < 2u; i++) {
        const unsigned said = said_as_master(k[i], k0[i], r0, PLINK_VFEED_STALE_MS + 1000u);
        if (getenv("PLINK_VOICE_SHOW") != NULL) {
            printf("master %u, %s %u, at most %llu ms later: %s\n", master, k[i]->name, said,
                   (unsigned long long)s_latest, said_text(&k[i]->v, k0[i]));
        }
        TEST_ASSERT_TRUE_MESSAGE(said * 4u >= master * 3u, said_text(&k[i]->v, k0[i]));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pairing_from_scratch_reaches_online);
    RUN_TEST(test_arm_away_entry_countdown_and_disarm_from_a_keypad);
    RUN_TEST(test_wrong_codes_lock_out_that_keypad_only);
    RUN_TEST(test_command_for_another_partition_is_refused);
    RUN_TEST(test_home_partition_moves_and_sounds_follow_it);
    RUN_TEST(test_a_lost_event_push_is_filled_in);
    RUN_TEST(test_events_pushed_before_a_new_session_are_kept);
    RUN_TEST(test_a_new_keypad_gets_the_newest_50_events);
    RUN_TEST(test_a_change_shows_on_every_keypad_within_a_second);
    RUN_TEST(test_master_restart_keypads_back_within_15_s);
    RUN_TEST(test_capabilities_offered_meet_in_the_and);
    RUN_TEST(test_keypad_restart_within_the_cache_runs_each_command_once);
    RUN_TEST(test_master_to_keypad_cut_lost_at_30_s_and_trouble_by_status_age);
    RUN_TEST(test_keypad_to_master_cut_trouble_after_the_supervision_time);
    RUN_TEST(test_keypad_switched_off_trouble_then_cleared_when_back);
    RUN_TEST(test_master_wifi_down_two_minutes_then_supervision_resumes);
    RUN_TEST(test_panic_during_an_outage_runs_once);
    RUN_TEST(test_panic_keyed_while_lost_runs_once);
    RUN_TEST(test_removal_says_bye_and_refuses_the_keypad);
    RUN_TEST(test_removed_keypad_pairs_again);
    RUN_TEST(test_eight_keypads_pull_the_configuration_through_loss);
    RUN_TEST(test_replays_after_either_side_restarts_are_refused);
    RUN_TEST(test_addresses_move_with_authenticated_frames);
    RUN_TEST(test_hostile_keypad_is_rate_limited_and_the_others_go_on);
    RUN_TEST(test_a_keypad_says_what_the_master_says);
    RUN_TEST(test_a_keypad_says_nothing_old);
    RUN_TEST(test_a_keypad_moved_home_speaks_for_its_new_partition);
    RUN_TEST(test_under_loss_a_keypad_never_says_more_than_the_master);
    RUN_TEST(test_six_hours_on_a_damaged_network);
    return UNITY_END();
}
