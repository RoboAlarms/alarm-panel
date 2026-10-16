/*
 * plink_session.h - the handshake at every connection and the sessions it makes (docs/features/11
 * "Sessions"; LINK-003, LINK-015): HELLO / WELCOME keyed by the pairing key with ephemeral X25519,
 * per-direction AES-128-CCM keys and a session id, sequence numbers in RAM, the replay window.
 *
 * The keypad starts (plink_hs_hello), the master answers (plink_hs_answer) and keeps the result as
 * a pending session until a sealed frame proves the keypad has it; the keypad derives the same
 * session from the WELCOME (plink_hs_finish). Data frames are sealed and opened with
 * plink_session_seal / plink_session_open.
 */
#ifndef PLINK_SESSION_H
#define PLINK_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "panel_link/plink_crypto.h"
#include "panel_link/plink_frame.h"
#include "panel_link/plink_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One session, one direction each way. Holds keys: wipe it (plink_session_wipe) when done. */
typedef struct {
    bool valid;
    uint32_t sid;
    uint8_t k_tx[PLINK_AES_LEN];
    uint8_t k_rx[PLINK_AES_LEN];
    uint32_t tx_seq;    /* the last seq sent (the next is tx_seq + 1) */
    plink_window_t rx;  /* the peer's seq seen */
    uint8_t proto;      /* the lower of the two sides' */
    uint16_t caps;      /* the common capabilities */
    plink_caps_t peer_cap; /* the keypad's capacities (master side) */
} plink_session_t;

void plink_session_wipe(plink_session_t *s);

/* Seals a payload as a frame of this session: seq = ++tx_seq (false, nothing sent, when that
 * would reach PLINK_SEQ_LIMIT); from_master sets the direction. */
PLINK_MUST_CHECK
bool plink_session_seal(const plink_crypto_t *c, plink_session_t *s, uint8_t type,
                        uint16_t net_id, uint16_t sender, bool from_master,
                        const uint8_t *payload, size_t len, uint8_t *out, size_t cap,
                        size_t *out_len);

/* Opens a sealed frame of this session (h->sid == s->sid): the replay window is checked first,
 * the tag next, and only then the seq committed. false: drop it. */
PLINK_MUST_CHECK
bool plink_session_open(const plink_crypto_t *c, plink_session_t *s, const plink_hdr_t *h,
                        const uint8_t *frame, size_t len, uint8_t *pt, size_t cap,
                        size_t *pt_len);

/* ---- the keypad's handshake ---------------------------------------------------------------- */
typedef struct {
    bool waiting;                 /* a HELLO is out and its WELCOME not yet taken */
    uint8_t e[PLINK_X25519_LEN];  /* ephemeral private key */
    uint8_t n_k[PLINK_NONCE_LEN];
    uint8_t hello[128];           /* the HELLO frame, for the transcript and resending */
    size_t hello_len;
} plink_hs_k_t;

/* A fresh HELLO (new e_k and n_k, never reused) from keypad `device` of net_id into hs->hello;
 * the frame to send is hs->hello. */
PLINK_MUST_CHECK
bool plink_hs_hello(const plink_crypto_t *c, const uint8_t k_pair[PLINK_KEY_LEN], uint16_t net_id,
                    uint16_t device, const plink_caps_t *cap, plink_hs_k_t *hs);

/* A WELCOME for the HELLO in hs: mac2 checked, the session derived into *out, e_k and n_k wiped,
 * hs->waiting cleared. false (out untouched): not an answer to this HELLO, or a bad MAC. */
PLINK_MUST_CHECK
bool plink_hs_finish(const plink_crypto_t *c, const uint8_t k_pair[PLINK_KEY_LEN],
                     plink_hs_k_t *hs, const plink_hdr_t *h, const uint8_t *frame, size_t len,
                     plink_session_t *out);

void plink_hs_k_wipe(plink_hs_k_t *hs);

/* ---- the master's answer ------------------------------------------------------------------- */

/* Checks a HELLO from a paired keypad (mac1 under its K_pair; the header's sender and net_id are
 * the caller's to match with the keypad first), then makes the WELCOME into welcome (cap bytes)
 * and the pending session. *n_k receives the HELLO's nonce (the caller drops HELLOs it has seen)
 * and *hello its decoded fields (capacities). false: drop the HELLO, nothing made. */
PLINK_MUST_CHECK
bool plink_hs_answer(const plink_crypto_t *c, const uint8_t k_pair[PLINK_KEY_LEN],
                     const plink_hdr_t *h, const uint8_t *frame, size_t len,
                     plink_session_t *pending, plink_hello_t *hello, uint8_t *welcome,
                     size_t cap, size_t *welcome_len);

/* Only checks mac1 (cheap, before any X25519): true when this HELLO was made with k_pair. */
bool plink_hs_hello_valid(const uint8_t k_pair[PLINK_KEY_LEN], const uint8_t *frame, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* PLINK_SESSION_H */
