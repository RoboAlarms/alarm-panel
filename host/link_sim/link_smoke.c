/*
 * link_smoke.c - the panel link end to end in one process (docs/features/11 LINK-013, "Test and
 * acceptance": CI's link_smoke). A virtual master (the real engine) and a virtual keypad, each
 * with its own real UDP socket on 127.0.0.1, on a virtual clock: 5 ms steps, both sockets looked
 * at with a zero timeout each step, and a datagram one side sent to the other is waited for
 * before the clock moves on, so the run never depends on how the OS schedules loopback traffic
 * and takes seconds for minutes of link time.
 *
 * It proves, in order: pairing (both confirm after checking the codes are equal); the keypad
 * syncs to ONLINE; arm away with the master code from the keypad and the keypad shows it;
 * disarm; a wrong code answered INVALID_CODE; Smart Home and settings on the keypad (BL-216,
 * LINK-044..051): the keypad mirrors the master's devices - its records the master's, byte for
 * byte - an unlock without a code is answered NEED_CODE at once with nothing sent, an unlock with
 * Alex's code is judged once on the master as the keypad's and the keypad shows the lock unlocked,
 * the keypad's settings report reaches the master and a settings write from the master is taken on
 * the keypad and answered with what it keeps; the master's installer menu from the keypad (BL-223,
 * LINK-058..062): through a ui_remote proxy on the keypad, installer mode entered with the
 * installer code and held by the keypad, the master's sensor source listened to, a new zone saved
 * in the master's configuration and bound to the sensor heard, installer mode left, and a save
 * refused once it isn't the keypad's; the keypad stopped (no heartbeats) raises the
 * master's supervision trouble after the supervision time and clears it when it is back
 * (LINK-007); the master restarted from its stored record has the keypad ONLINE again within
 * 15 s (docs/features/11 "Test and acceptance") and its devices mirrored again; a removed keypad
 * is told and is NOT_PAIRED with its record erased (LINK-014); it pairs again asking by
 * broadcast, as link_sim does without mDNS (here the test's LAN hands a broadcast to the master's
 * socket); no AES-CCM key and nonce was used twice on either side (LINK-003).
 *
 * The crypto port is the seeded test port (deterministic), so the sockets are loopback only
 * (link_udp.h). Exit 0: passed; 1: failed (the message says where); 77: no UDP socket here
 * (CTest's SKIP_RETURN_CODE). -v prints both sides' logs.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "alarm/alarm_strings.h"
#include "link_node.h"
#include "link_udp.h"
#include "plink_test_port.h"

#define STEP_MS       5u
#define SUPERVISION_S 60u /* the shortest of the choices a person has (LINK-007) */
#define SKIP          77

typedef struct {
    const char *who;
    link_udp_t udp;
    unsigned inflight; /* datagrams sent to this socket and not taken yet */
    bool down;         /* switched off: whatever reaches it is thrown away */
    plink_test_port_t tp;
    plink_crypto_t c;
    link_store_t store;
} side_t;

static side_t SM = {.who = "master"};
static side_t SK = {.who = "keypad"};
static link_master_t M;
static link_keypad_t K;
static uint64_t NOW = 1000000u; /* the engine's clock starts at 1000 s, as in the harness */
static bool VERBOSE;
static unsigned LOST;
static const char *STAGE = "start";
static const uint8_t LO[4] = {127, 0, 0, 1};
static const uint8_t BCAST[4] = {255, 255, 255, 255};

static int fail(const char *fmt, ...) LINK_PRINTF(1, 2);
static bool step(void);

/* BL-223: the keypad's screen's calls of the master's pages - a ui_remote proxy whose port
 * starts the call on the keypad's link manager and runs the network until it is answered, as a
 * panel's call task waits for its link task. */
static ui_remote_proxy_t RPX;
static ui_backend_t RBE;
static ui_remote_status_t rb_call(void *ctx, uint16_t call, const uint8_t *args, size_t len,
                                  uint8_t *ans, size_t cap, size_t *ans_len);
static uint32_t rb_cfg_crc(void *ctx);

static void say_m(void *ctx, const char *line)
{
    (void)ctx;
    printf("%8.3f  M  %s\n", (double)(NOW - 1000000u) / 1000.0, line);
}

static void say_k(void *ctx, const char *line)
{
    (void)ctx;
    printf("%8.3f  K  %s\n", (double)(NOW - 1000000u) / 1000.0, line);
}

/* port.send of either side: the datagram goes out on that side's socket for real. The test's
 * LAN is the two sockets: a broadcast (to the other side's port) reaches the other side. */
static bool side_send(void *ctx, const plink_addr_t *to, const uint8_t *frame, size_t len)
{
    side_t *s = ctx;
    if (s->down || to->tp != PLINK_TP_UDP || to->len < 6u) {
        return s->down; /* a switched-off side sends nothing */
    }
    side_t *peer = s == &SM ? &SK : &SM;
    const uint16_t port = (uint16_t)(((unsigned)to->a[4] << 8) | (unsigned)to->a[5]);
    uint8_t ip[4];
    memcpy(ip, to->a, 4);
    if (memcmp(ip, BCAST, 4) == 0 && peer->udp.open) {
        memcpy(ip, peer->udp.ip, 4);
    }
    if (!link_udp_send(&s->udp, ip, port, frame, len)) {
        return false;
    }
    if (peer->udp.open && memcmp(ip, peer->udp.ip, 4) == 0 && port == peer->udp.port) {
        peer->inflight++;
    }
    return true;
}

static void deliver(side_t *s, const uint8_t ip[4], uint16_t port, const uint8_t *buf, size_t len)
{
    if (s->inflight > 0u) {
        s->inflight--;
    }
    if (s->down) {
        return;
    }
    const plink_addr_t from = plink_addr_udp(ip, port);
    if (s == &SM) {
        link_master_input(&M, NOW, &from, buf, len);
    } else {
        link_keypad_input(&K, NOW, &from, buf, len);
    }
}

/* Takes what both sockets have; false on a socket error. */
static bool drain(side_t *s, bool *got)
{
    if (!s->udp.open) {
        return true; /* switched off with its socket closed (the master's restart) */
    }
    uint8_t buf[PLINK_FRAME_MAX];
    for (;;) {
        uint8_t ip[4];
        uint16_t port = 0;
        const int n = link_udp_recv(&s->udp, buf, sizeof(buf), ip, &port, 0u);
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            return true;
        }
        *got = true;
        deliver(s, ip, port, buf, (size_t)n);
    }
}

/* Delivers every datagram in flight between the two, waiting for each in real time. */
static bool pump(void)
{
    for (unsigned rounds = 0; rounds < 10000u; rounds++) {
        bool got = false;
        if (!drain(&SM, &got) || !drain(&SK, &got)) {
            return false;
        }
        if (got) {
            continue;
        }
        side_t *s = SM.inflight > 0u ? &SM : (SK.inflight > 0u ? &SK : NULL);
        if (s == NULL) {
            return true;
        }
        uint8_t buf[PLINK_FRAME_MAX];
        uint8_t ip[4];
        uint16_t port = 0;
        const int n = link_udp_recv(&s->udp, buf, sizeof(buf), ip, &port, 1000u);
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            LOST += s->inflight; /* loopback lost it: the protocol has to cope, as on a LAN */
            if (VERBOSE) {
                printf("         %u datagram(s) to the %s never arrived\n", s->inflight, s->who);
            }
            s->inflight = 0u;
            continue;
        }
        deliver(s, ip, port, buf, (size_t)n);
    }
    return false;
}

/* One 5 ms step of the virtual clock. */
static bool step(void)
{
    NOW += STEP_MS;
    if (!SM.down) {
        link_master_tick(&M, NOW);
    }
    if (!SK.down) {
        link_keypad_tick(&K, NOW);
        link_keypad_report(&K, NOW);
    }
    return pump();
}

typedef bool (*cond_fn)(void);

/* Runs until cond holds (true) or ms of virtual time passed (false). */
static bool run_until(cond_fn cond, uint64_t ms)
{
    const uint64_t end = NOW + ms;
    while (!cond()) {
        if (NOW >= end || !step()) {
            return false;
        }
    }
    return true;
}

static void run_for(uint64_t ms)
{
    const uint64_t end = NOW + ms;
    while (NOW < end && step()) {
    }
}

/* ---- what the run waits for ---------------------------------------------------------------- */

static plink_keypad_info_t kinfo(void)
{
    plink_keypad_info_t in;
    plink_keypad_info(&K.k, NOW, &in);
    return in;
}

static bool peer(uint8_t device, plink_peer_info_t *out)
{
    plink_peer_info_t p[PLINK_KEYPADS_MAX];
    const size_t n = plink_master_peers(&M.m, NOW, p, PLINK_KEYPADS_MAX);
    for (size_t i = 0; i < n; i++) {
        if (p[i].device == device) {
            *out = p[i];
            return true;
        }
    }
    return false;
}

static uint8_t DEVICE;

static bool both_compare(void)
{
    plink_pair_info_t m, k;
    plink_master_pair_info(&M.m, NOW, &m);
    plink_keypad_pair_info(&K.k, NOW, &k);
    return m.state == PLINK_PS_COMPARE && k.state == PLINK_PS_COMPARE;
}

static bool keypad_online(void)
{
    return kinfo().state == PLINK_LINK_ONLINE;
}

static bool keypad_online_both_ways(void)
{
    plink_peer_info_t p;
    return keypad_online() && peer(DEVICE, &p) && p.online && !p.down;
}

static bool keypad_not_paired(void)
{
    return kinfo().state == PLINK_LINK_NOT_PAIRED;
}

static bool trouble_raised(void)
{
    return DEVICE >= 1u && DEVICE <= 32u &&
           (alarm_engine_device_troubles(&M.eng) & (1u << (DEVICE - 1u))) != 0u;
}

static bool back_and_clear(void)
{
    return !trouble_raised() && keypad_online_both_ways();
}

static uint32_t REQ;
static plink_result_t RES;

static bool answered(void)
{
    const plink_cmd_state_t st = plink_keypad_command_state(&K.k, REQ, &RES);
    return st == PLINK_CMD_DONE || st == PLINK_CMD_NO_ANSWER;
}

static alarm_arm_state_t WANT_STATE;
static alarm_arm_level_t WANT_LEVEL;

/* The keypad mirrors the master's devices: the list it was last handed, every record in, fresh,
 * and the records the master's byte for byte (their states_crc). */
static bool keypad_has_devices(void)
{
    plink_dev_info_t di;
    plink_keypad_dev_info(&K.k, NOW, &di);
    if (!di.shared || !di.fresh || di.list_crc == 0u || di.list_crc != M.dev_list_crc ||
        di.count != M.dev_count) {
        return false;
    }
    for (uint8_t i = 0; i < di.count; i++) {
        plink_dev_view_t d;
        if (!plink_keypad_dev_view(&K.k, NOW, i, &d) || !d.have_rec) {
            return false;
        }
    }
    return plink_home_states_crc(&K.mirror.recs) == plink_home_states_crc(&M.drecs);
}

/* Row ref as the keypad's screen would show it (false: no such row). */
static bool keypad_row(const char *ref, plink_dev_view_t *out)
{
    plink_dev_info_t di;
    plink_keypad_dev_info(&K.k, NOW, &di);
    for (uint8_t i = 0; i < di.count; i++) {
        if (plink_keypad_dev_view(&K.k, NOW, i, out) && out->usable &&
            strcmp(out->cat.ref, ref) == 0) {
            return true;
        }
    }
    return false;
}

static const char *WANT_WORD;

static bool keypad_lock_shows(void)
{
    plink_dev_view_t d;
    return keypad_row("lock.front_door", &d) && d.online && !d.pending && !d.failed &&
           strcmp(d.word, WANT_WORD) == 0;
}

static uint32_t DEV_TICKET;

static bool dev_answered(void)
{
    const plink_cmd_state_t st = plink_keypad_dev_act_state(&K.k, DEV_TICKET, NULL);
    return st == PLINK_CMD_DONE || st == PLINK_CMD_NO_ANSWER;
}

static bool master_has_report(void)
{
    plink_keypad_home_t kh;
    return plink_master_keypad_home(&M.m, NOW, DEVICE, &kh) && kh.prefs.have &&
           strcmp(kh.prefs.panel_id, "1c2d3e") == 0;
}

static unsigned WRITES_ENDED;

static bool write_ended(void)
{
    return M.pset_end.count > WRITES_ENDED;
}

static bool keypad_shows(void)
{
    const alarm_snapshot_t *s = plink_keypad_snapshot(&K.k);
    const uint8_t p = link_keypad_home(&K, NOW);
    return (s->partitions & (1u << (p - 1u))) != 0u && s->part[p - 1u].state == WANT_STATE &&
           s->part[p - 1u].level == WANT_LEVEL;
}

/* ---- the run ------------------------------------------------------------------------------- */

static uint32_t CFG_SEQ;

static bool grant_arrived(void)
{
    plink_rb_grant_t g;
    return plink_keypad_rb_grant(&K.k, &g) && (g.areas & UR_AREA_ZONES) != 0u;
}

static bool keypad_has_new_config(void)
{
    return plink_keypad_config_seq(&K.k) != CFG_SEQ;
}

static ui_remote_status_t rb_call(void *ctx, uint16_t call, const uint8_t *args, size_t len,
                                  uint8_t *ans, size_t cap, size_t *ans_len)
{
    (void)ctx;
    *ans_len = 0u;
    uint32_t req = 0;
    if (!plink_keypad_rb_start(&K.k, NOW, call, args, len, &req)) {
        return UR_UNAVAILABLE;
    }
    const uint64_t end = NOW + 12000u;
    uint8_t st = PLINK_RB_PENDING;
    size_t n = 0u;
    while ((st = plink_keypad_rb_poll(&K.k, req, &n)) == PLINK_RB_PENDING && NOW < end) {
        if (!step()) {
            break;
        }
    }
    if (st == PLINK_RB_OK) {
        if (n > cap) {
            return UR_TOO_BIG;
        }
        memcpy(ans, K.rb_ans, n);
        *ans_len = n;
    }
    return st == PLINK_RB_PENDING ? UR_NO_ANSWER : (ui_remote_status_t)st;
}

static uint32_t rb_cfg_crc(void *ctx)
{
    (void)ctx;
    return plink_keypad_cfg_crc(&K.k);
}

static int fail(const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    printf("link_smoke: FAIL at \"%s\" (%.3f s of link time): %s\n", STAGE,
           (double)(NOW - 1000000u) / 1000.0, msg);
    /* What both sides think now, for the CI log. */
    M.io.say = say_m;
    K.io.say = say_k;
    link_master_say_status(&M, NOW);
    link_master_say_keypads(&M, NOW);
    link_keypad_say_status(&K, NOW);
    fflush(stdout);
    return 1;
}

static bool open_side(side_t *s, uint64_t seed, uint16_t port)
{
    plink_test_port_init(&s->tp, seed);
    s->c = plink_test_port(&s->tp);
    s->inflight = 0u;
    /* The seeded port is not secure: loopback only (docs/features/11 "Design"). */
    return link_udp_open(&s->udp, LO, port, false, s->c.insecure);
}

/* A command from the keypad, waited for; its result into RES. */
static bool keypad_command(alarm_cmd_type_t type, alarm_arm_level_t level, const char *code)
{
    plink_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = PLINK_OP_COMMAND;
    cmd.cmd.type = type;
    cmd.cmd.partition = link_keypad_home(&K, NOW);
    cmd.cmd.level = level;
    snprintf(cmd.cmd.code, sizeof(cmd.cmd.code), "%s", code);
    REQ = plink_keypad_command(&K.k, NOW, &cmd);
    memset(&cmd, 0, sizeof(cmd));
    if (REQ == 0u) {
        return false;
    }
    memset(&RES, 0, sizeof(RES));
    const bool ok = run_until(answered, 5000u) &&
                    plink_keypad_command_state(&K.k, REQ, &RES) == PLINK_CMD_DONE;
    plink_keypad_command_release(&K.k, REQ);
    return ok;
}

int main(int argc, char **argv)
{
    VERBOSE = argc > 1 && strcmp(argv[1], "-v") == 0;
    const uint64_t real0 = link_os_mono_ms();
    if (!link_udp_startup()) {
        printf("link_smoke: SKIP: no sockets here\n");
        return SKIP;
    }
    if (!open_side(&SM, 0x5EED0001u, 0u) || !open_side(&SK, 0x5EED0002u, 0u)) {
        printf("link_smoke: SKIP: no UDP socket on 127.0.0.1: %s\n",
               SM.udp.open ? SK.udp.error : SM.udp.error);
        return SKIP;
    }
    link_store_init(&SM.store, NULL);
    link_store_init(&SK.store, NULL);
    const link_io_t io_m = {.send = side_send, .send_ctx = &SM, .say = VERBOSE ? say_m : NULL};
    const link_io_t io_k = {.send = side_send, .send_ctx = &SK, .say = VERBOSE ? say_k : NULL};
    const plink_addr_t maddr = plink_addr_udp(SM.udp.ip, SM.udp.port);

    STAGE = "start";
    if (!plink_crypto_check(&SM.c)) {
        return fail("plink_crypto_check failed on the test port");
    }
    if (!link_master_init(&M, &SM.c, &io_m, &SM.store, "Smoke master", "5a0c3e", NOW)) {
        return fail("the virtual master did not start");
    }
    link_keypad_init(&K, &SK.c, &io_k, &SK.store, "Smoke keypad", "1c2d3e", &maddr, NOW);
    run_for(1000u);
    if (kinfo().state != PLINK_LINK_NOT_PAIRED) {
        return fail("a new keypad is %s, not \"not paired\"", link_state_text(kinfo().state));
    }

    /* LINK-004: pairing, both screens showing the same code, confirmed on both. */
    STAGE = "pairing";
    if (!plink_master_pair_open(&M.m, NOW)) {
        return fail("pairing did not open");
    }
    link_master_sync(&M, NOW);
    plink_keypad_pair_start(&K.k, NOW, &maddr, false, 0u);
    if (!run_until(both_compare, 30000u)) {
        return fail("the two sides never showed a code to compare");
    }
    plink_pair_info_t pm, pk;
    plink_master_pair_info(&M.m, NOW, &pm);
    plink_keypad_pair_info(&K.k, NOW, &pk);
    if (strlen(pm.code) != 6u || strcmp(pm.code, pk.code) != 0) {
        return fail("the codes differ: master \"%s\", keypad \"%s\"", pm.code, pk.code);
    }
    char code[8];
    link_code_text(pm.code, code);
    plink_keypad_pair_confirm(&K.k, NOW, true);
    plink_master_pair_decide(&M.m, NOW, true);
    link_master_sync(&M, NOW);

    /* LINK-008: the keypad syncs (session, status, configuration) to ONLINE. */
    STAGE = "sync";
    const uint64_t paired_at = NOW;
    if (!run_until(keypad_online, 30000u)) {
        return fail("the keypad never came ONLINE after pairing (it is %s)",
                    link_state_text(kinfo().state));
    }
    const uint64_t online_ms = NOW - paired_at;
    DEVICE = kinfo().device;
    plink_peer_info_t p;
    if (DEVICE < PLINK_DEVICE_FIRST || DEVICE > PLINK_DEVICE_LAST || !peer(DEVICE, &p)) {
        return fail("the master doesn't list keypad %u", (unsigned)DEVICE);
    }
    if (SM.store.len == 0u || SK.store.len == 0u) {
        return fail("a side stored nothing at pairing (master %u bytes, keypad %u bytes)",
                    (unsigned)SM.store.len, (unsigned)SK.store.len);
    }
    if (plink_keypad_config(&K.k) == NULL) {
        return fail("ONLINE without a configuration");
    }
    if (!run_until(keypad_online_both_ways, 5000u)) {
        return fail("the master doesn't see keypad %u online", (unsigned)DEVICE);
    }

    /* BL-216, LINK-044 and LINK-045: the master's devices mirrored on the keypad. */
    STAGE = "devices";
    if (!run_until(keypad_has_devices, 10000u)) {
        plink_dev_info_t di;
        plink_keypad_dev_info(&K.k, NOW, &di);
        return fail("the keypad didn't mirror the master's %u devices (list %08x: it has %u of "
                    "list %08x, %s)",
                    (unsigned)M.dev_count, (unsigned)M.dev_list_crc, (unsigned)di.count,
                    (unsigned)di.list_crc, di.fresh ? "fresh" : "not fresh");
    }
    const uint64_t devices_ms = NOW - (paired_at + online_ms);

    /* LINK-002: the master checks the code; the keypad shows the master's state. */
    STAGE = "arm away";
    if (!keypad_command(ALARM_CMD_ARM, ALARM_LEVEL_AWAY, ALARM_DEFAULT_MASTER_CODE)) {
        return fail("no answer to arm away");
    }
    if (RES.result != ALARM_OK) {
        return fail("arm away: %s", alarm_result_name((alarm_result_t)RES.result));
    }
    WANT_STATE = ALARM_STATE_EXIT_DELAY;
    WANT_LEVEL = ALARM_LEVEL_AWAY;
    if (!run_until(keypad_shows, 1000u)) {
        return fail("the keypad doesn't show the exit delay of an away arming");
    }
    alarm_partition_status_t ps;
    alarm_engine_partition_status(&M.eng, link_keypad_home(&K, NOW), &ps);
    if (ps.state != ALARM_STATE_EXIT_DELAY || ps.armed_by != 2u) {
        return fail("the master's engine is %s, armed by user %u", alarm_state_name(ps.state),
                    (unsigned)ps.armed_by);
    }

    STAGE = "disarm";
    if (!keypad_command(ALARM_CMD_DISARM, ALARM_LEVEL_NONE, ALARM_DEFAULT_MASTER_CODE)) {
        return fail("no answer to disarm");
    }
    if (RES.result != ALARM_OK) {
        return fail("disarm: %s", alarm_result_name((alarm_result_t)RES.result));
    }
    WANT_STATE = ALARM_STATE_DISARMED;
    WANT_LEVEL = ALARM_LEVEL_NONE;
    if (!run_until(keypad_shows, 1000u)) {
        return fail("the keypad doesn't show the partition disarmed");
    }

    STAGE = "wrong code";
    if (!keypad_command(ALARM_CMD_ARM, ALARM_LEVEL_AWAY, "0000")) {
        return fail("no answer to a wrong code");
    }
    if (RES.result != ALARM_ERR_INVALID_CODE) {
        return fail("a wrong code was answered %s", alarm_result_name((alarm_result_t)RES.result));
    }
    run_for(500u);
    WANT_STATE = ALARM_STATE_DISARMED;
    if (!keypad_shows()) {
        return fail("a wrong code changed the state");
    }

    /* BL-216, LINK-045: still the master's devices, as they are. */
    STAGE = "device state";
    WANT_WORD = "locked";
    if (!keypad_has_devices() || M.dev_count < 6u || !keypad_lock_shows()) {
        return fail("the keypad doesn't show the house's Front Door locked");
    }

    /* LINK-047 and LINK-048: an unlock from the keypad, judged once on the master as the keypad's
     * for its home partition. Without a code it is answered at once, with nothing sent. */
    STAGE = "device action";
    const unsigned judged = M.dev_runs;
    uint32_t none = 0;
    if (plink_keypad_dev_act(&K.k, NOW, "lock.front_door", ACT_UNLOCK, 0, 0u, NULL, &none) !=
            PLINK_DEV_NEED_CODE ||
        none != 0u) {
        return fail("an unlock without a code wasn't answered NEED_CODE at once");
    }
    run_for(200u);
    if (M.dev_runs != judged) {
        return fail("an unlock without a code reached the master");
    }
    const uint64_t act_at = NOW;
    DEV_TICKET = link_keypad_dev_act(&K, NOW, "lock.front_door", ACT_UNLOCK, 0, 0u, "1111");
    if (DEV_TICKET == 0u) {
        return fail("the keypad refused an unlock with Alex's code at once");
    }
    if (!run_until(dev_answered, 5000u)) {
        return fail("no answer to the unlock in 5 s");
    }
    plink_dev_answer_t ans;
    if (plink_keypad_dev_act_state(&K.k, DEV_TICKET, &ans) != PLINK_CMD_DONE ||
        ans.result != PLINK_DEV_OK) {
        return fail("the unlock was answered %s, not done", link_dev_result_name(ans.result));
    }
    WANT_WORD = "unlocked";
    if (!run_until(keypad_lock_shows, 3000u)) {
        return fail("the keypad doesn't show Front Door unlocked");
    }
    const uint64_t act_ms = NOW - act_at;
    const hc_device_t *lock = link_master_device(&M, "lock.front_door");
    if (M.dev_runs != judged + 1u || lock == NULL || strcmp(lock->info.state, "unlocked") != 0) {
        return fail("the master judged the unlock %u times and its lock is %s", M.dev_runs - judged,
                    lock != NULL ? lock->info.state : "gone");
    }

    /* LINK-050 and LINK-051: the keypad's settings at the master, and a write from it. */
    STAGE = "settings";
    if (!run_until(master_has_report, 5000u)) {
        return fail("the keypad's settings report never reached the master");
    }
    WRITES_ENDED = M.pset_end.count;
    const uint64_t set_at = NOW;
    if (!link_master_prefs_set(&M, NOW, DEVICE, PROTO_PREFS_F_BRIGHTNESS, 60u)) {
        return fail("the master didn't send a settings change to keypad %u", (unsigned)DEVICE);
    }
    if (!run_until(write_ended, 10000u)) {
        return fail("the settings change never ended");
    }
    const uint64_t set_ms = NOW - set_at;
    uint32_t kept = 0, mine = 0;
    if (M.pset_end.result != PLINK_PSET_OK || M.pset_end.req != M.pset_req ||
        M.pset_end.refused_count != 0u ||
        !proto_prefs_get(&M.pset_end.values, PROTO_PREFS_F_BRIGHTNESS, &kept) || kept != 60u) {
        return fail("the settings change ended %u with %u refused, brightness %u",
                    (unsigned)M.pset_end.result, (unsigned)M.pset_end.refused_count,
                    (unsigned)kept);
    }
    if (!proto_prefs_get(&K.prefs, PROTO_PREFS_F_BRIGHTNESS, &mine) || mine != 60u ||
        K.prefs_applied != 1u) {
        return fail("the keypad's brightness is %u after %u change(s)", (unsigned)mine,
                    K.prefs_applied);
    }

    /* LINK-007: a keypad that stops is a supervision trouble after its supervision time. */
    /* BL-223, LINK-058..062: the master's installer menu from the keypad. */
    STAGE = "installer from the keypad";
    if (!run_until(grant_arrived, 3000u)) {
        return fail("the master never told the keypad which of its settings it serves");
    }
    RPX.port = (ui_remote_port_t){.ctx = NULL, .call = rb_call, .cfg_crc = rb_cfg_crc};
    plink_rb_grant_t g;
    (void)plink_keypad_rb_grant(&K.k, &g);
    ui_remote_proxy(&RBE, &RPX, g.areas);
    if (RBE.program_enter == NULL || RBE.zone_bind == NULL || RBE.set_config == NULL) {
        return fail("the master serves areas %08x: no installer menu", (unsigned)g.areas);
    }
    uint8_t iuser = 0;
    alarm_result_t ir = RBE.program_enter(RBE.ctx, "1234", &iuser);
    if (ir != ALARM_ERR_INVALID_CODE || M.eng.program) {
        return fail("a wrong installer code from the keypad was answered %s", alarm_result_name(ir));
    }
    ir = RBE.program_enter(RBE.ctx, ALARM_DEFAULT_INSTALLER_CODE, &iuser);
    if (ir != ALARM_OK || !M.eng.program || M.eng.program_src.kind != ALARM_SRC_KEYPAD ||
        M.eng.program_src.id != DEVICE) {
        return fail("installer mode from the keypad: %s (held by %u/%u)", alarm_result_name(ir),
                    (unsigned)M.eng.program_src.kind, (unsigned)M.eng.program_src.id);
    }
    ui_zone_source_t srcs[4];
    if (RBE.zone_sources(RBE.ctx, srcs, 4) != 1u || srcs[0].kind != ZSRC_SIM || !srcs[0].usable) {
        return fail("the master's sensor sources didn't reach the keypad");
    }
    RBE.zone_learn(RBE.ctx, ZSRC_SIM, true);
    link_mrb_heard(&M, 5u); /* someone opens the sensor on input 5 */
    zsrc_candidate_t cands[8];
    const size_t nc = RBE.zone_candidates(RBE.ctx, ZSRC_SIM, "", cands, 8);
    size_t heard = 99u;
    for (size_t i = 0; i < nc; i++) {
        heard = cands[i].changed ? i : heard;
    }
    if (nc != 8u || heard >= nc || cands[heard].addr.id != 5u) {
        return fail("the keypad didn't see the sensor heard on the master (%u inputs)", (unsigned)nc);
    }
    /* the zone itself, in the master's configuration: from the keypad's copy */
    static alarm_config_t ncfg;
    ncfg = *plink_keypad_config(&K.k);
    alarm_zone_config_defaults(&ncfg.zones[4], 5u, ALARM_ZT_PERIMETER, 1u);
    snprintf(ncfg.zones[4].name, sizeof(ncfg.zones[4].name), "Back Door");
    const uint32_t cfg_seq = plink_keypad_config_seq(&K.k);
    ir = RBE.set_config(RBE.ctx, &ncfg);
    if (ir != ALARM_OK || strcmp(alarm_engine_config(&M.eng)->zones[4].name, "Back Door") != 0) {
        return fail("the new zone from the keypad: %s (UR status %u)", alarm_result_name(ir),
                    (unsigned)RPX.last);
    }
    if (alarm_engine_config(&M.eng)->users[1].code_hash[1] == 0xA5u) {
        return fail("a keypad's configuration replaced the master's codes");
    }
    zone_hub_binding_t zb;
    memset(&zb, 0, sizeof(zb));
    zb.in[ZSRC_ROLE_MAIN].addr = cands[heard].addr;
    if (RBE.zone_bind(RBE.ctx, 5u, &zb) != ZONE_HUB_OK) {
        return fail("zone 5's sensor couldn't be set from the keypad");
    }
    zone_hub_info_t zi;
    if (!RBE.zone_info(RBE.ctx, 5u, &zi) || strcmp(zi.address, "Input 5") != 0) {
        return fail("zone 5's sensor isn't on the master");
    }
    RBE.zone_learn(RBE.ctx, ZSRC_SIM, false);
    if (M.rb.learning) {
        return fail("the master still listens after the keypad stopped");
    }
    /* a Z-Wave device paired on the master's radio from the keypad (LINK-062) */
    ui_zw_status_t zs;
    if (!RBE.zw_status(RBE.ctx, &zs) || zs.home_id != 0xC0FFEE01u) {
        return fail("the master's Z-Wave radio didn't reach the keypad");
    }
    ui_zw_request_t zq;
    memset(&zq, 0, sizeof(zq));
    zq.kind = UI_ZW_REQ_ADD;
    if (RBE.zw_request(RBE.ctx, &zq) != UI_ZW_REQ_OK) {
        return fail("adding a Z-Wave device from the keypad was refused");
    }
    ui_zw_progress_t zp;
    RBE.zw_progress(RBE.ctx, &zp);
    if (zp.step != UI_ZW_STEP_LISTENING || zp.seconds_left == 0u) {
        return fail("the master's radio isn't listening (step %u)", (unsigned)zp.step);
    }
    link_mrb_zw_join(&M); /* the installer presses the sensor's button */
    run_for(100u);
    RBE.zw_progress(RBE.ctx, &zp);
    if (zp.step != UI_ZW_STEP_DONE || zp.result != UI_ZW_RES_ADDED || zp.node != 5u) {
        return fail("the device didn't join the master (step %u, result %u)", (unsigned)zp.step,
                    (unsigned)zp.result);
    }
    ui_zw_device_t zd[4];
    if (RBE.zw_devices(RBE.ctx, zd, 4) != 1u || strcmp(zd[0].model, "Door sensor") != 0) {
        return fail("the keypad doesn't list the master's new Z-Wave device");
    }
    zq.kind = UI_ZW_REQ_DISMISS;
    (void)RBE.zw_request(RBE.ctx, &zq);
    CFG_SEQ = cfg_seq;
    if (!run_until(keypad_has_new_config, 3000u) ||
        strcmp(plink_keypad_config(&K.k)->zones[4].name, "Back Door") != 0) {
        return fail("the keypad's copy of the configuration lacks the new zone");
    }
    RBE.program_exit(RBE.ctx);
    if (M.eng.program) {
        return fail("installer mode stayed on after the keypad left it");
    }
    ir = RBE.set_config(RBE.ctx, &ncfg);
    if (ir != ALARM_ERR_NOT_AUTHORIZED) {
        return fail("a save outside installer mode was answered %s", alarm_result_name(ir));
    }
    printf("  installer from the keypad: %u calls served on the master\n", M.rb.runs);

    STAGE = "supervision";
    if (!plink_master_set_supervision(&M.m, NOW, DEVICE, (uint16_t)SUPERVISION_S)) {
        return fail("supervision %u s refused", SUPERVISION_S);
    }
    link_master_sync(&M, NOW);
    run_for(2000u);
    SK.down = true;
    const uint64_t stopped = NOW;
    if (!run_until(trouble_raised, (uint64_t)SUPERVISION_S * 3000u)) {
        return fail("no supervision trouble %u s after the keypad stopped", 3u * SUPERVISION_S);
    }
    const uint64_t trouble_ms = NOW - stopped;
    if (trouble_ms + 12000u < (uint64_t)SUPERVISION_S * 1000u) {
        return fail("the trouble came after %.1f s, before the supervision time (%u s)",
                    (double)trouble_ms / 1000.0, SUPERVISION_S);
    }
    if ((M.down & (1u << (DEVICE - 1u))) == 0u || !peer(DEVICE, &p) || !p.down) {
        return fail("the engine has the trouble but the link doesn't say the keypad is down");
    }

    STAGE = "keypad back";
    SK.down = false;
    const uint64_t resumed = NOW;
    if (!run_until(back_and_clear, 60000u)) {
        return fail("the keypad came back but the trouble stayed (keypad %s, trouble %s)",
                    link_state_text(kinfo().state), trouble_raised() ? "raised" : "clear");
    }
    const uint64_t back_ms = NOW - resumed;

    /* The master restarts from its stored record: the keypad is ONLINE again within 15 s. */
    STAGE = "master restart";
    if (SM.tp.reused) {
        return fail("the master used an AES-CCM key and nonce twice");
    }
    const uint16_t mport = SM.udp.port;
    SM.down = true;
    link_udp_close(&SM.udp);
    SM.inflight = 0u;
    run_for(3000u); /* the master is off for 3 s */
    if (!open_side(&SM, 0x5EED0003u, mport)) {
        return fail("the master's port %u could not be bound again: %s", (unsigned)mport,
                    SM.udp.error);
    }
    SM.down = false;
    if (!link_master_init(&M, &SM.c, &io_m, &SM.store, "Smoke master", "5a0c3e", NOW)) {
        return fail("the virtual master did not restart");
    }
    if (!peer(DEVICE, &p)) {
        return fail("the restarted master lost keypad %u", (unsigned)DEVICE);
    }
    const uint64_t restarted = NOW;
    if (!run_until(keypad_online_both_ways, 15000u)) {
        return fail("the keypad was not ONLINE with the restarted master within 15 s (%s)",
                    link_state_text(kinfo().state));
    }
    const uint64_t restart_ms = NOW - restarted;
    if (trouble_raised()) {
        return fail("the restarted master raised a trouble for keypad %u", (unsigned)DEVICE);
    }
    /* Its house again from the start (the lock locked): mirrored again on the keypad. */
    WANT_WORD = "locked";
    if (!run_until(keypad_has_devices, 15000u) || !keypad_lock_shows()) {
        return fail("the keypad didn't mirror the restarted master's devices");
    }

    /* LINK-014: removed at the master, the keypad is told and forgets the master. */
    STAGE = "remove";
    const uint8_t first_device = DEVICE;
    if (!plink_master_remove(&M.m, NOW, DEVICE)) {
        return fail("plink_master_remove refused keypad %u", (unsigned)DEVICE);
    }
    link_master_sync(&M, NOW);
    if (!run_until(keypad_not_paired, 5000u)) {
        return fail("the keypad didn't hear it was removed (it is %s)",
                    link_state_text(kinfo().state));
    }
    if (kinfo().unpaired_why != PLINK_UNPAIRED_REMOVED) {
        return fail("NOT_PAIRED, but not because it was removed");
    }
    if (SK.store.len != 0u || SK.store.erases == 0u) {
        return fail("the keypad kept its record");
    }
    run_for(3000u); /* the BYEs are over */
    if (peer(DEVICE, &p) || plink_master_held(&M.m) != 0u) {
        return fail("the master still holds keypad %u", (unsigned)DEVICE);
    }
    plink_master_rec_t rec;
    if (!plink_store_master_decode(SM.store.buf, SM.store.len, &rec) || rec.count != 0u) {
        return fail("the master's stored record still has a keypad");
    }
    plink_wipe(&rec, sizeof(rec));
    if (trouble_raised()) {
        return fail("a removed keypad left its trouble behind");
    }

    /* Paired again, asking by broadcast: the first master to answer is paired with. */
    STAGE = "pairing by broadcast";
    if (!plink_master_pair_open(&M.m, NOW)) {
        return fail("pairing did not open again");
    }
    link_master_sync(&M, NOW);
    const plink_addr_t bcast = plink_addr_udp(BCAST, SM.udp.port);
    const uint64_t asked = NOW;
    link_keypad_pair(&K, NOW, &bcast);
    if (!run_until(both_compare, 30000u)) {
        return fail("asked by broadcast, the two sides never showed a code to compare");
    }
    plink_master_pair_info(&M.m, NOW, &pm);
    plink_keypad_pair_info(&K.k, NOW, &pk);
    if (strlen(pm.code) != 6u || strcmp(pm.code, pk.code) != 0 ||
        !plink_addr_equal(&pk.peer, &maddr)) {
        return fail("asked by broadcast: codes \"%s\" and \"%s\"", pm.code, pk.code);
    }
    plink_keypad_pair_confirm(&K.k, NOW, true);
    plink_master_pair_decide(&M.m, NOW, true);
    link_master_sync(&M, NOW);
    if (!run_until(keypad_online, 30000u)) {
        return fail("paired by broadcast, the keypad never came ONLINE (it is %s)",
                    link_state_text(kinfo().state));
    }
    DEVICE = kinfo().device;
    if (!run_until(keypad_online_both_ways, 5000u)) {
        return fail("paired by broadcast, the master doesn't see keypad %u online",
                    (unsigned)DEVICE);
    }
    const uint64_t broadcast_ms = NOW - asked;

    STAGE = "nonces";
    if (SM.tp.reused || SK.tp.reused) {
        return fail("an AES-CCM key and nonce were used twice (%s)",
                    SM.tp.reused ? "master" : "keypad");
    }

    printf("link_smoke: OK - paired (code %s) and ONLINE in %.1f s; arm away, disarm and a wrong "
           "code from keypad %u; %u devices mirrored %.1f s after ONLINE, an unlock shown "
           "unlocked %.1f s after it was keyed, a settings change answered in %.1f s; "
           "supervision trouble %.1f s after it stopped, cleared %.1f s after it was back; ONLINE "
           "%.1f s after the master restarted; removed; paired again by broadcast and ONLINE in "
           "%.1f s. %.0f s of link time in %u ms%s\n",
           code, (double)online_ms / 1000.0, (unsigned)first_device, (unsigned)M.dev_count,
           (double)devices_ms / 1000.0, (double)act_ms / 1000.0, (double)set_ms / 1000.0,
           (double)trouble_ms / 1000.0, (double)back_ms / 1000.0, (double)restart_ms / 1000.0,
           (double)broadcast_ms / 1000.0, (double)(NOW - 1000000u) / 1000.0,
           (unsigned)(link_os_mono_ms() - real0),
           LOST != 0u ? " (some loopback datagrams were lost)" : "");
    link_udp_close(&SM.udp);
    link_udp_close(&SK.udp);
    link_udp_cleanup();
    return 0;
}
