/*
 * pair.c - the panel's pairing state machine (proto_pair.h, HAI-003): one attempt per
 * 2-minute window, commit before reveal, the code shown only after the commitment verified.
 * Portable and host-tested; ha_link wires transport, randomness and storage around it.
 */
#include <string.h>

#include "alarm_proto/proto_pair.h"

/* Forget the secrets but keep what the caller still reads (state, fail, peer_fp on DONE). */
static void wipe(proto_pair_t *p)
{
    memset(p->commit, 0, sizeof(p->commit));
    memset(p->nonce_panel, 0, sizeof(p->nonce_panel));
    memset(p->code, 0, sizeof(p->code));
}

void proto_pair_open(proto_pair_t *p, uint64_t now_ms, const uint8_t own_fp[PROTO_LINK_FP_LEN])
{
    memset(p, 0, sizeof(*p));
    p->state = PROTO_PAIR_WINDOW;
    p->deadline_ms = now_ms + PROTO_PAIR_WINDOW_MS;
    memcpy(p->own_fp, own_fp, PROTO_LINK_FP_LEN);
}

void proto_pair_close(proto_pair_t *p)
{
    memset(p, 0, sizeof(*p));
    p->state = PROTO_PAIR_IDLE;
}

void proto_pair_tick(proto_pair_t *p, uint64_t now_ms)
{
    const bool running = p->state == PROTO_PAIR_WINDOW || p->state == PROTO_PAIR_WAIT_REVEAL ||
                         p->state == PROTO_PAIR_SHOW_CODE;
    if (running && now_ms >= p->deadline_ms) {
        wipe(p);
        p->state = PROTO_PAIR_FAILED;
        p->fail = PROTO_LINK_R_TIMEOUT;
    }
}

bool proto_pair_on_start(proto_pair_t *p, uint64_t now_ms,
                         const uint8_t commit[PROTO_LINK_NONCE_LEN],
                         const uint8_t peer_fp[PROTO_LINK_FP_LEN],
                         const uint8_t nonce_panel[PROTO_LINK_NONCE_LEN], const char **reply)
{
    proto_pair_tick(p, now_ms);
    if (p->state == PROTO_PAIR_WAIT_REVEAL || p->state == PROTO_PAIR_SHOW_CODE) {
        *reply = PROTO_LINK_R_BUSY; /* the running attempt goes on undisturbed */
        return false;
    }
    if (p->state != PROTO_PAIR_WINDOW) {
        *reply = p->fail != NULL ? p->fail : PROTO_LINK_R_NOT_PAIRING;
        return false;
    }
    memcpy(p->commit, commit, PROTO_LINK_NONCE_LEN);
    memcpy(p->peer_fp, peer_fp, PROTO_LINK_FP_LEN);
    memcpy(p->nonce_panel, nonce_panel, PROTO_LINK_NONCE_LEN);
    p->state = PROTO_PAIR_WAIT_REVEAL;
    return true;
}

bool proto_pair_on_reveal(proto_pair_t *p, uint64_t now_ms,
                          const uint8_t nonce_ha[PROTO_LINK_NONCE_LEN], const char **reply)
{
    proto_pair_tick(p, now_ms);
    if (p->state != PROTO_PAIR_WAIT_REVEAL) {
        *reply = p->fail != NULL ? p->fail : PROTO_LINK_R_NOT_PAIRING;
        return false;
    }
    uint8_t digest[32];
    proto_sha256(nonce_ha, PROTO_LINK_NONCE_LEN, digest);
    if (memcmp(digest, p->commit, sizeof(digest)) != 0) {
        wipe(p);
        p->state = PROTO_PAIR_FAILED;
        p->fail = PROTO_LINK_R_MISMATCH; /* the client didn't commit to that nonce */
        *reply = PROTO_LINK_R_MISMATCH;
        return false;
    }
    proto_link_pair_code(nonce_ha, p->nonce_panel, p->peer_fp, p->own_fp, p->code);
    p->state = PROTO_PAIR_SHOW_CODE;
    return true;
}

const char *proto_pair_decide(proto_pair_t *p, bool allow)
{
    if (p->state != PROTO_PAIR_SHOW_CODE) {
        return PROTO_LINK_R_NOT_PAIRING;
    }
    wipe(p);
    if (allow) {
        p->state = PROTO_PAIR_DONE; /* peer_fp is what the caller pins */
        return PROTO_LINK_R_PAIRED;
    }
    p->state = PROTO_PAIR_FAILED;
    p->fail = PROTO_LINK_R_REJECTED;
    return PROTO_LINK_R_REJECTED;
}
