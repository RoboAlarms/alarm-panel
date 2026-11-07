/*
 * tls_gate.h - who may start a TLS handshake with one of the panel's servers, decided before
 * mbedTLS allocates anything (SEC-012, docs/bugs/B033). A handshake is the expensive part of a
 * connection, and anyone on the home network can ask for one without signing in: a flood of them
 * must cost the panel a closed socket each, never the internal RAM its Wi-Fi co-processor needs.
 *
 * Each address has a bucket of handshakes that refills over time; a handshake that fails costs more.
 * All addresses together share a second bucket, so many addresses can't add up to a flood. A new
 * handshake is also refused while internal RAM is short. The panel has one gate, shared by every
 * TLS listener (the Panel Manager, the Home Assistant link, the developer tools): a flood of one
 * counts against all of them.
 *
 * The bucket logic is portable: time and memory come in as arguments (host test:
 * test/test_tls_gate.c). tls_gate_fd() and the rest at the end are the firmware's shared gate.
 */
#ifndef TLS_GATE_H
#define TLS_GATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TLS_GATE_PEERS 12u
/* One address: a browser opens about six connections for a page, and a phone that reconnects every
 * second or two never runs dry. */
#define TLS_GATE_PEER_BURST 8u
#define TLS_GATE_PEER_REFILL_US 500000 /* one handshake back every half second */
/* A failed handshake (a stalled one holds its server for the whole timeout) uses this many: one
 * that fails every time gets a try every three seconds. */
#define TLS_GATE_FAIL_COST 6u
/* Every address together. */
#define TLS_GATE_ALL_BURST 16u
#define TLS_GATE_ALL_REFILL_US 200000
/* Internal RAM a new handshake leaves alone: what ESP-Hosted's SDIO buffers, lwIP and the server
 * link need to keep the panel on the network. */
#define TLS_GATE_MIN_FREE 32768u
#define TLS_GATE_MIN_BLOCK 8192u
/* An address that proved who it is (a Panel Manager session, a paired phone or Home Assistant)
 * skips the shared bucket this long after (F20): a flood from other addresses can't crowd it out.
 * Its own bucket still holds, and it is the last to lose its slot. */
#define TLS_GATE_TRUST_US 600000000

typedef enum {
    TLS_GATE_ADMIT = 0,
    TLS_GATE_PEER_LIMIT, /* this address used its handshakes */
    TLS_GATE_ALL_LIMIT,  /* everyone together did */
    TLS_GATE_LOW_MEMORY, /* internal RAM is short */
} tls_gate_verdict_t;

typedef struct {
    uint8_t addr[16]; /* IPv6, or IPv4-mapped */
    bool used;
    uint16_t tokens; /* in whole handshakes */
    int64_t refill_at_us;
    int64_t seen_us;
    int64_t trusted_until_us; /* 0: not trusted */
} tls_gate_peer_t;

typedef struct {
    tls_gate_peer_t peer[TLS_GATE_PEERS];
    uint16_t all_tokens;
    int64_t all_refill_at_us;
    bool started;
    uint32_t counts[4]; /* by verdict: [TLS_GATE_ADMIT] admitted, the others refused */
} tls_gate_t;

/* May addr start a handshake now? internal_free and internal_block are the heap's free internal
 * bytes and its largest free internal block. Takes one token from both buckets when admitted. */
tls_gate_verdict_t tls_gate_admit(tls_gate_t *g, const uint8_t addr[16], int64_t now_us,
                                  size_t internal_free, size_t internal_block);
/* The handshake admitted for addr failed (garbage, a timeout, a scanner): it costs more. */
void tls_gate_failed(tls_gate_t *g, const uint8_t addr[16], int64_t now_us);
/* addr proved who it is over a connection the gate let in (F20). */
void tls_gate_trust(tls_gate_t *g, const uint8_t addr[16], int64_t now_us);
const char *tls_gate_verdict_name(tls_gate_verdict_t v);

/* ---- the firmware's one gate (tls_gate_esp.c) -------------------------------------------------
 * For a socket just accepted: true to go on with the handshake, false to close it now. A refusal
 * is logged at most once every ten seconds, with how many there were. who names the server. */
bool tls_gate_fd(int fd, const char *who);
/* The handshake on fd failed. */
void tls_gate_fd_failed(int fd);
/* The client on fd proved who it is (signed in, a paired phone or Home Assistant). */
void tls_gate_fd_trust(int fd);
/* The counts since start, by verdict. */
void tls_gate_counts(uint32_t out[4]);

#endif
