/*
 * proto_pair.h - the panel's side of pairing (HAI-003, protocol.md "Pairing"): a portable
 * state machine over proto_link messages. The caller owns transport, randomness, clocks and
 * the screen; this decides what is allowed when, verifies the commitment and derives the code.
 *
 * One attempt per window: after a failure the window is closed and the user starts over.
 */
#ifndef PROTO_PAIR_H
#define PROTO_PAIR_H

#include "alarm_proto/proto_link.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_PAIR_WINDOW_MS (2u * 60u * 1000u) /* HAI-003: pairing opens for 2 minutes */

typedef enum {
    PROTO_PAIR_IDLE = 0,   /* window closed */
    PROTO_PAIR_WINDOW,     /* open, waiting for a client's pair_start */
    PROTO_PAIR_WAIT_REVEAL,/* nonce sent, waiting for the client's pair_reveal */
    PROTO_PAIR_SHOW_CODE,  /* both sides show the code; waiting for Allow on the panel */
    PROTO_PAIR_DONE,       /* allowed: pin peer_fp and store */
    PROTO_PAIR_FAILED,     /* see fail; the window is closed */
} proto_pair_state_t;

typedef struct {
    proto_pair_state_t state;
    uint64_t deadline_ms;                     /* when the window closes */
    uint8_t commit[PROTO_LINK_NONCE_LEN];     /* the client's commitment */
    uint8_t nonce_panel[PROTO_LINK_NONCE_LEN];
    uint8_t own_fp[PROTO_LINK_FP_LEN];        /* the panel certificate's fingerprint */
    uint8_t peer_fp[PROTO_LINK_FP_LEN];       /* the client certificate's fingerprint */
    char code[PROTO_LINK_CODE_LEN];           /* "" until PROTO_PAIR_SHOW_CODE */
    const char *fail;                         /* a PROTO_LINK_R_* string when FAILED */
} proto_pair_t;

/* Open the 2-minute window (the user asked on the panel). own_fp = this panel's certificate. */
void proto_pair_open(proto_pair_t *p, uint64_t now_ms,
                     const uint8_t own_fp[PROTO_LINK_FP_LEN]);

/* The user closed the window, or the link dropped mid-attempt. */
void proto_pair_close(proto_pair_t *p);

/* Call with the current time before reading state: closes the window when time is up. */
void proto_pair_tick(proto_pair_t *p, uint64_t now_ms);

/* A client sent pair_start. peer_fp = its certificate's fingerprint, nonce_panel = fresh
 * random bytes from the caller. true: answer with proto_link_pair_nonce(nonce_panel).
 * false: answer with proto_link_pair_result(reply) and close the connection. */
bool proto_pair_on_start(proto_pair_t *p, uint64_t now_ms,
                         const uint8_t commit[PROTO_LINK_NONCE_LEN],
                         const uint8_t peer_fp[PROTO_LINK_FP_LEN],
                         const uint8_t nonce_panel[PROTO_LINK_NONCE_LEN],
                         const char **reply);

/* The client revealed its nonce. true: both screens now show p->code (PROTO_PAIR_SHOW_CODE).
 * false: the commitment didn't verify or the moment was wrong; answer with
 * proto_link_pair_result(reply) and close the connection (the window is closed too). */
bool proto_pair_on_reveal(proto_pair_t *p, uint64_t now_ms,
                          const uint8_t nonce_ha[PROTO_LINK_NONCE_LEN], const char **reply);

/* The user compared the codes on both screens. allow=true: PROTO_PAIR_DONE (pin p->peer_fp,
 * store, answer "paired"). false: PROTO_PAIR_FAILED (answer "rejected"). Returns the
 * PROTO_LINK_R_* string to send. */
const char *proto_pair_decide(proto_pair_t *p, bool allow);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_PAIR_H */
