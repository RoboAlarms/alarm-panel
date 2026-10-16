/*
 * plink_pair.h - pairing a keypad with its master (docs/features/11 "Pairing"; LINK-004,
 * LINK-016): the master's and the keypad's state machines over PAIR_* frames, commit-then-reveal
 * with a 6-digit code compared on both screens.
 *
 * Each side takes whole frames (already accepted by plink_hdr_read) and answers with at most one
 * frame (plink_out_t). Neither side stores anything: when both confirmations verified the master
 * hands its manager the keypad to store (plink_pm_take, then plink_pm_stored builds PAIR_DONE),
 * and the keypad hands its manager the master (plink_pk_take). Time comes in as ms.
 *
 * The rules that keep a device in the middle to one guess per attempt (LINK-016):
 *   - within an attempt each value is set once: a repeat that is byte-identical to the message
 *     already taken for that step gets the stored answer again; a different one ends the attempt
 *     (PLINK_FAIL_INTERRUPTED) and never replaces a stored value;
 *   - an attempt counts once its secret is disclosed - the master's when it sends PAIR_REVEAL,
 *     the keypad's when it sends PAIR_NONCE - however it ends; 3 per window (master) or per
 *     pairing started by a person (keypad), at least 3 s apart;
 *   - within one window every attempt comes from one address: another address ends the window
 *     (PLINK_FAIL_OVERLAP);
 *   - PAIR_FAIL is honoured only for the current attempt and from its peer's address.
 */
#ifndef PLINK_PAIR_H
#define PLINK_PAIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "panel_link/plink_crypto.h"
#include "panel_link/plink_frame.h"
#include "panel_link/plink_msg.h"
#include "panel_link/plink_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLINK_PAIR_WINDOW_MS     120000u /* LINK-016 */
#define PLINK_PAIR_STEP_MS       30000u
#define PLINK_PAIR_COMPARE_MS    120000u
#define PLINK_PAIR_ATTEMPTS      3u
#define PLINK_PAIR_GAP_MS        3000u
#define PLINK_PAIR_DONE_KEEP_MS  60000u /* an identical PAIR_CONFIRM gets the same PAIR_DONE */
#define PLINK_PAIR_RESEND_MS     1000u  /* the keypad's first resend; doubles to ... */
#define PLINK_PAIR_RESEND_MAX_MS 4000u
#define PLINK_CODE_LEN 7u                /* six digits and the NUL */

/* One frame to send, or none (len 0). */
typedef struct {
    plink_addr_t to;
    size_t len;
    uint8_t buf[PLINK_FRAME_MAX];
} plink_out_t;

/* The 6-digit code (docs/features/11 "Pairing" step 5): "000000".."999999". */
void plink_pair_code(const uint8_t pk_k[PLINK_X25519_LEN], const uint8_t pk_m[PLINK_X25519_LEN],
                     const uint8_t n_k[PLINK_NONCE_LEN], const uint8_t n_m[PLINK_NONCE_LEN],
                     char code[PLINK_CODE_LEN]);

/* The commitment (step 3). */
void plink_pair_commitment(const uint8_t pk_m[PLINK_X25519_LEN],
                           const uint8_t pk_k[PLINK_X25519_LEN],
                           const uint8_t n_m[PLINK_NONCE_LEN], uint8_t out[PLINK_SHA256_LEN]);

/* The keys (step 6): K_pair and K_conf from the shared secret and the transcript. */
PLINK_MUST_CHECK
bool plink_pair_keys(const uint8_t dh[PLINK_X25519_LEN], const uint8_t *req_payload,
                     size_t req_len, const uint8_t *commit_payload, size_t commit_len,
                     uint16_t net_id, const uint8_t n_k[PLINK_NONCE_LEN],
                     const uint8_t n_m[PLINK_NONCE_LEN], uint8_t k_pair[PLINK_KEY_LEN],
                     uint8_t k_conf[PLINK_KEY_LEN]);

/* A frame ending in a MAC: the last 16 bytes become HMAC(key, everything before)[0..15]. */
void plink_mac_fill(const uint8_t key[PLINK_KEY_LEN], uint8_t *frame, size_t len);
/* true when the last 16 bytes are that MAC (constant time). */
bool plink_mac_check(const uint8_t key[PLINK_KEY_LEN], const uint8_t *frame, size_t len);

/* What either side shows (the pairing screens and the `plink` console). */
typedef enum {
    PLINK_PS_IDLE = 0,  /* nothing under way */
    PLINK_PS_OPEN,      /* master: window open, waiting for a keypad; keypad: request sent */
    PLINK_PS_WORKING,   /* keys being exchanged (commit, nonce, reveal) */
    PLINK_PS_COMPARE,   /* the code is shown: waiting for the person's answer */
    PLINK_PS_WAITING,   /* this side said yes: waiting for the other side */
    PLINK_PS_DONE,      /* paired */
    PLINK_PS_FAILED,    /* see fail */
} plink_pair_state_t;

typedef struct {
    uint8_t state;      /* plink_pair_state_t */
    uint8_t fail;       /* plink_fail_t when FAILED */
    uint8_t attempts;   /* attempts used in this window / pairing */
    uint32_t left_s;    /* master: the window's seconds left; keypad: the step's */
    char code[PLINK_CODE_LEN];     /* "" unless COMPARE or WAITING */
    char peer_name[PLINK_NAME_MAX + 1]; /* the other side's name (as sent, cleaned) */
    plink_addr_t peer;  /* the other side's address */
    uint8_t device;     /* DONE: the keypad's number */
} plink_pair_info_t;

/* ---- the master's side --------------------------------------------------------------------- */
typedef struct {
    /* private */
    const plink_crypto_t *c;
    uint16_t net_id;
    char name[PLINK_NAME_MAX + 1];
    char panel_id[PLINK_PANEL_ID_LEN + 1];
    uint16_t zones_used; /* the master's: a keypad that can't hold them is refused */
    uint8_t state;       /* plink_pair_state_t */
    uint8_t fail;
    uint8_t attempts;
    bool window;
    uint64_t window_until, step_until, next_attempt_at;
    bool have_peer;      /* the window's one address */
    plink_addr_t peer;
    uint32_t attempt;
    bool allowed, confirmed, revealed;
    uint8_t sk[PLINK_X25519_LEN], pk_m[PLINK_X25519_LEN], n_m[PLINK_NONCE_LEN];
    uint8_t pk_k[PLINK_X25519_LEN], n_k[PLINK_NONCE_LEN];
    plink_pair_req_t req;
    uint8_t req_raw[160];    size_t req_len;
    uint8_t commit_raw[160]; size_t commit_len;
    uint8_t nonce_raw[32];   size_t nonce_len;
    uint8_t confirm_raw[32]; size_t confirm_len;
    uint8_t k_pair[PLINK_KEY_LEN], k_conf[PLINK_KEY_LEN];
    char code[PLINK_CODE_LEN];
    uint8_t device, home;
    uint8_t done_frame[64];  size_t done_len; uint64_t done_until;
    plink_addr_t done_peer;
} plink_pair_m_t;

/* A keypad ready to be stored (plink_pm_take). */
typedef struct {
    uint8_t k_pair[PLINK_KEY_LEN];
    uint8_t kind;
    plink_caps_t cap;
    char name[PLINK_NAME_MAX + 1];
} plink_pair_new_t;

void plink_pm_init(plink_pair_m_t *pm, const plink_crypto_t *c);
/* The master's identity for PAIR_COMMIT, and its zones for the capacity check. */
void plink_pm_identity(plink_pair_m_t *pm, uint16_t net_id, const char *name,
                       const char panel_id[PLINK_PANEL_ID_LEN + 1], uint16_t zones_used);
/* Opens the 2-minute window (the installer asked, in installer mode); resets the attempt count. */
void plink_pm_open(plink_pair_m_t *pm, uint64_t now_ms);
/* The installer cancelled: PAIR_FAIL CANCELLED to the peer of an attempt under way. */
void plink_pm_cancel(plink_pair_m_t *pm, uint64_t now_ms, plink_out_t *out);
/* A PAIR_* frame from a keypad (h from plink_hdr_read; frame the whole frame). */
void plink_pm_input(plink_pair_m_t *pm, uint64_t now_ms, const plink_addr_t *from,
                    const plink_hdr_t *h, const uint8_t *frame, size_t len, plink_out_t *out);
/* Timers: the window, a step, the comparison. */
void plink_pm_tick(plink_pair_m_t *pm, uint64_t now_ms, plink_out_t *out);
/* The installer compared the codes: allow or reject. */
void plink_pm_decide(plink_pair_m_t *pm, uint64_t now_ms, bool allow, plink_out_t *out);
/* true once allowed and confirmed: the keypad to store. The manager then picks the number,
 * stores, and calls plink_pm_stored. */
bool plink_pm_take(const plink_pair_m_t *pm, plink_pair_new_t *out);
/* The manager stored it as device (ok) or couldn't (no number: PLINK_FAIL_FULL; storage:
 * PLINK_FAIL_STORE): PAIR_DONE or PAIR_FAIL; the secrets are wiped, the window closes. */
void plink_pm_stored(plink_pair_m_t *pm, uint64_t now_ms, bool ok, uint8_t fail, uint8_t device,
                     uint8_t home, plink_out_t *out);
void plink_pm_info(const plink_pair_m_t *pm, uint64_t now_ms, plink_pair_info_t *out);
/* The earliest time a timer needs tick (UINT64_MAX none). */
uint64_t plink_pm_next(const plink_pair_m_t *pm);

/* ---- the keypad's side --------------------------------------------------------------------- */
typedef struct {
    /* private */
    const plink_crypto_t *c;
    uint8_t kind;
    plink_caps_t cap;
    char name[PLINK_NAME_MAX + 1];
    char fw[PLINK_FW_MAX + 1];
    uint8_t state;
    uint8_t fail;
    uint8_t attempts;
    uint64_t step_until, resend_at, next_attempt_at;
    uint32_t resend_ms;
    plink_addr_t master;
    uint16_t net_id;
    uint32_t attempt;
    uint8_t sk[PLINK_X25519_LEN], pk_k[PLINK_X25519_LEN], n_k[PLINK_NONCE_LEN];
    uint8_t pk_m[PLINK_X25519_LEN], n_m[PLINK_NONCE_LEN];
    uint8_t commit[PLINK_SHA256_LEN];
    plink_pair_commit_t commit_msg;
    uint8_t req_raw[160];    size_t req_len;
    uint8_t commit_raw[160]; size_t commit_len;
    uint8_t last[PLINK_FRAME_MAX]; size_t last_len; /* the frame being resent */
    uint8_t k_pair[PLINK_KEY_LEN], k_conf[PLINK_KEY_LEN];
    char code[PLINK_CODE_LEN];
    bool nonce_sent, revealed, confirmed;
    plink_keypad_rec_t result;
} plink_pair_k_t;

/* name: what the keypad calls itself; fw its version; kind and capacities as in PAIR_REQ. */
void plink_pk_init(plink_pair_k_t *pk, const plink_crypto_t *c, uint8_t kind,
                   const plink_caps_t *cap, const char *name, const char *fw);
/* A person started pairing with the master at that address (from discovery: its TXT net is
 * net_id, 0 when not known): a new pairing of up to 3 attempts; sends PAIR_REQ. */
void plink_pk_start(plink_pair_k_t *pk, uint64_t now_ms, const plink_addr_t *master,
                    uint16_t net_id, plink_out_t *out);
/* Try again after a failure (counts against the 3 when the last one disclosed n_k). */
void plink_pk_retry(plink_pair_k_t *pk, uint64_t now_ms, plink_out_t *out);
void plink_pk_input(plink_pair_k_t *pk, uint64_t now_ms, const plink_addr_t *from,
                    const plink_hdr_t *h, const uint8_t *frame, size_t len, plink_out_t *out);
/* Resends and timeouts. */
void plink_pk_tick(plink_pair_k_t *pk, uint64_t now_ms, plink_out_t *out);
/* The person compared the codes: match sends PAIR_CONFIRM, otherwise PAIR_FAIL REFUSED. */
void plink_pk_confirm(plink_pair_k_t *pk, uint64_t now_ms, bool match, plink_out_t *out);
void plink_pk_cancel(plink_pair_k_t *pk, uint64_t now_ms, plink_out_t *out);
/* true once PAIR_DONE verified: the master to store (then the manager starts a handshake). The
 * secrets are wiped after this. */
bool plink_pk_take(plink_pair_k_t *pk, plink_keypad_rec_t *out);
void plink_pk_info(const plink_pair_k_t *pk, uint64_t now_ms, plink_pair_info_t *out);
uint64_t plink_pk_next(const plink_pair_k_t *pk);

#ifdef __cplusplus
}
#endif

#endif /* PLINK_PAIR_H */
