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
 * disarm; a wrong code answered INVALID_CODE; the keypad stopped (no heartbeats) raises the
 * master's supervision trouble after the supervision time and clears it when it is back
 * (LINK-007); the master restarted from its stored record has the keypad ONLINE again within
 * 15 s (docs/features/11 "Test and acceptance"); a removed keypad is told and is NOT_PAIRED with
 * its record erased (LINK-014); no AES-CCM key and nonce was used twice on either side (LINK-003).
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

static int fail(const char *fmt, ...) LINK_PRINTF(1, 2);

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

static bool to_side(const plink_addr_t *to, const side_t *s)
{
    if (to->tp != PLINK_TP_UDP || to->len < 6u || !s->udp.open) {
        return false;
    }
    const uint16_t port = (uint16_t)(((unsigned)to->a[4] << 8) | (unsigned)to->a[5]);
    return memcmp(to->a, s->udp.ip, 4) == 0 && port == s->udp.port;
}

/* port.send of either side: the datagram goes out on that side's socket for real. */
static bool side_send(void *ctx, const plink_addr_t *to, const uint8_t *frame, size_t len)
{
    side_t *s = ctx;
    if (s->down || to->tp != PLINK_TP_UDP || to->len < 6u) {
        return s->down; /* a switched-off side sends nothing */
    }
    side_t *peer = s == &SM ? &SK : &SM;
    const uint16_t port = (uint16_t)(((unsigned)to->a[4] << 8) | (unsigned)to->a[5]);
    if (!link_udp_send(&s->udp, to->a, port, frame, len)) {
        return false;
    }
    if (to_side(to, peer)) {
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

static bool keypad_shows(void)
{
    const alarm_snapshot_t *s = plink_keypad_snapshot(&K.k);
    const uint8_t p = link_keypad_home(&K, NOW);
    return (s->partitions & (1u << (p - 1u))) != 0u && s->part[p - 1u].state == WANT_STATE &&
           s->part[p - 1u].level == WANT_LEVEL;
}

/* ---- the run ------------------------------------------------------------------------------- */

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
    link_keypad_init(&K, &SK.c, &io_k, &SK.store, "Smoke keypad", &maddr, NOW);
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
    plink_keypad_pair_start(&K.k, NOW, &maddr, 0u);
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

    /* LINK-007: a keypad that stops is a supervision trouble after its supervision time. */
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

    /* LINK-014: removed at the master, the keypad is told and forgets the master. */
    STAGE = "remove";
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

    STAGE = "nonces";
    if (SM.tp.reused || SK.tp.reused) {
        return fail("an AES-CCM key and nonce were used twice (%s)",
                    SM.tp.reused ? "master" : "keypad");
    }

    printf("link_smoke: OK - paired (code %s) and ONLINE in %.1f s; arm away, disarm and a wrong "
           "code from keypad %u; supervision trouble %.1f s after it stopped, cleared %.1f s after "
           "it was back; ONLINE %.1f s after the master restarted; removed. %.0f s of link time "
           "in %u ms%s\n",
           code, (double)online_ms / 1000.0, (unsigned)DEVICE, (double)trouble_ms / 1000.0,
           (double)back_ms / 1000.0, (double)restart_ms / 1000.0,
           (double)(NOW - 1000000u) / 1000.0, (unsigned)(link_os_mono_ms() - real0),
           LOST != 0u ? " (some loopback datagrams were lost)" : "");
    link_udp_close(&SM.udp);
    link_udp_close(&SK.udp);
    link_udp_cleanup();
    return 0;
}
